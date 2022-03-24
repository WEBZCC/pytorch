from lazy_tensor_core import _LAZYC
import dataclasses
from typing import List, Dict, Any, Callable
import copy
from torch import fx
import torch

debug = False

@dataclasses.dataclass
class GraphInputMatcher:
    """
    The GraphInputMatcher class setup the graph inputs for future calls after lazy tracing.
    Specifically, those graph inputs corresponding to method parameters should be replaced with the
    arguments for the current call.

    tensor_id_to_arg_idx maps the tensor id to the parameter index.
    graph_input_tensor_ids, graph_input_ivalues list the tensor_id and ivalue for each of the
    TS/XLA graph inputs.
    """
    tensor_id_to_arg_idx: Dict[int, int]
    graph_input_tensor_ids: List[int]
    # there are 2 categories of graph_input_tensors.
    # Category 1: those whose id are not found in tensor_id_to_arg_idx. These are
    # most likely const tensors and we can get its content from graph_input_tensors
    # Category 2: those whose id are found in tensor_id_to_arg_idx. We should get
    #  the tensor from method arguments
    graph_input_ivalues: List[Any]

    # get the real graph input tensors
    def __call__(self, args):
        real_input = []
        for tensor_id, traced_ivalue in zip(self.graph_input_tensor_ids, self.graph_input_ivalues):
            arg_idx = self.tensor_id_to_arg_idx.get(tensor_id, None)
            if arg_idx is None:
                inp = traced_ivalue
            else:
                inp = args[arg_idx]
            real_input.append(inp)
        return real_input

class ReturnValueHandler:
    r"""
    When _LAZYC._ltc_sync_multi is called on multi tensors, the compiled graph
    will contain output only for unique tensors - if a tensor appears multiple
    times in the input to _ltc_sync_multi, only the first occurance matters.

    However from python level, we still expect multi tensors returned with duplciation
    even if the TS graph dedup the output. e.g. for method:

      def forward(self, a):
        return a, a

    the TS graph captured by LTC will return a single tensor, but Python method expects 2.

    This class dedup the lazy tensors first to get the index that will be used
    to duplicate the eager tensors later.
    """
    def __init__(self, lazy_out_list):
        self.index = []
        self.total_count = len(lazy_out_list)

        tensor_id_to_idx = dict()
        for dup_idx, lazy_tensor in enumerate(lazy_out_list):
            uniq_idx = tensor_id_to_idx.get(id(lazy_tensor), None)
            if uniq_idx is not None:
                self.index[uniq_idx].append(dup_idx)
            else:
                uniq_idx = len(self.index)
                self.index.append([dup_idx])
                tensor_id_to_idx[id(lazy_tensor)] = uniq_idx

    def duplicate_eager_tensors(self, eager_tensor_list):
        duplicated_list = [None] * self.total_count
        assert len(eager_tensor_list) == len(self.index)

        for uniq_idx, eager_tensor in enumerate(eager_tensor_list):
            for dup_idx in self.index[uniq_idx]:
                duplicated_list[dup_idx] = eager_tensor
        return duplicated_list

def force_lazy_device(model: fx.GraphModule):
    """
    Factory methods in a Fx graph may create tensors for a specific eager devices.
    If we take no actions, those eager tensors will be mixed with lazy tensors and
    cause crash. This method overwrite those eager device to lazy device.
    """
    def tolazydevice(dev):
        if isinstance(dev, torch.device):
            return torch.device("lazy", index=dev.index)
        return dev

    for nd in model.graph.nodes:
        nd.args = tuple(tolazydevice(arg) for arg in nd.args)
        nd.kwargs = {k: tolazydevice(v) for k, v in nd.kwargs.items()}
    model.recompile()


def extract_compiled_graph(model: fx.GraphModule, example_inputs) -> Callable:
    """
    Optimize an eager model with LTC and returns a wrapper to execute the
    compiled graph directly without retracing. It depends on other mechanisms
    like TorchDynamo guards to guarantee the returned wrapper is only called
    when it's safe.
    """
    lazy_args = [arg.to(device="lazy") for arg in example_inputs]
    args_tensor_ids = [_LAZYC._ltc_get_tensor_id(lazy_arg) for lazy_arg in lazy_args]
    tensor_id_to_arg_idx = {tensor_id: i for i, tensor_id in enumerate(args_tensor_ids)}
    lazy_model = copy.deepcopy(model).to(device="lazy")
    force_lazy_device(lazy_model)

    # This line executes lazy tracing and enable us extracting compiled graph later
    lazy_out = lazy_model(*lazy_args)
    if not isinstance(lazy_out, (tuple, list)):
        lazy_out = (lazy_out,)

    return_value_handler = ReturnValueHandler(lazy_out)
    if debug:
        print("LTC IR:", _LAZYC._get_ltc_tensors_text(lazy_out))

    graph_input_tensor_ids, graph_input_ivalues = _LAZYC._get_ltc_tensors_ts_device_data_node(lazy_out)
    assert len(graph_input_tensor_ids) == len(graph_input_ivalues)
    graph_input_matcher = GraphInputMatcher(tensor_id_to_arg_idx, graph_input_tensor_ids, graph_input_ivalues)

    graph_hash = _LAZYC._get_graph_hash(lazy_out)

    if debug:
        print("graph_hash", graph_hash)
        print(f"args_tensor_ids {args_tensor_ids}")
        print("tensor ids from device data:", graph_input_tensor_ids)

    # sync the list of output tensors so the computation graph for these
    # tensors will be cached. Those computation graphs can be retrieved
    # by graph hash later.
    _LAZYC._ltc_sync_multi(lazy_out, [])

    def optimized_mod(*args):
        if len(lazy_out) == 0:
            return ()
        graph_input = graph_input_matcher(args)
        return return_value_handler.duplicate_eager_tensors(_LAZYC._run_cached_graph(graph_hash, graph_input))

    return optimized_mod