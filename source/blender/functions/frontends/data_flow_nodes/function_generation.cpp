#include "function_generation.hpp"
#include "graph_generation.hpp"

#include "FN_tuple_call.hpp"
#include "FN_dependencies.hpp"
#include "FN_llvm.hpp"
#include "DNA_node_types.h"

namespace FN {
namespace DataFlowNodes {

static void find_interface_sockets(VirtualNodeTree &vtree,
                                   VTreeDataGraph &data_graph,
                                   VectorSet<DataSocket> &r_inputs,
                                   VectorSet<DataSocket> &r_outputs)
{
  VirtualNode *input_node = vtree.nodes_with_idname("fn_FunctionInputNode").get(0, nullptr);
  VirtualNode *output_node = vtree.nodes_with_idname("fn_FunctionOutputNode").get(0, nullptr);

  if (input_node != nullptr) {
    for (uint i = 0; i < input_node->outputs().size() - 1; i++) {
      VirtualSocket *vsocket = input_node->output(i);
      r_inputs.add_new(data_graph.lookup_socket(vsocket));
    }
  }

  if (output_node != nullptr) {
    for (uint i = 0; i < output_node->inputs().size() - 1; i++) {
      VirtualSocket *vsocket = output_node->input(i);
      r_outputs.add_new(data_graph.lookup_socket(vsocket));
    }
  }
}

Optional<std::unique_ptr<Function>> generate_function(bNodeTree *btree)
{
  auto vtree = make_unique<VirtualNodeTree>();
  vtree->add_all_of_tree(btree);
  vtree->freeze_and_index();

  auto optional_data_graph = generate_graph(*vtree);
  if (!optional_data_graph.has_value()) {
    return {};
  }

  std::unique_ptr<VTreeDataGraph> data_graph = optional_data_graph.extract();

  VectorSet<DataSocket> input_sockets;
  VectorSet<DataSocket> output_sockets;
  find_interface_sockets(*vtree, *data_graph, input_sockets, output_sockets);

  FunctionGraph fgraph(data_graph->graph(), input_sockets, output_sockets);

  // fgraph.graph().to_dot__clipboard();

  std::unique_ptr<Function> fn = fgraph.new_function(btree->id.name);
  fgraph_add_DependenciesBody(*fn, fgraph);
  fgraph_add_LLVMBuildIRBody(*fn, fgraph);

  fgraph_add_TupleCallBody(*fn, fgraph);
  // derive_TupleCallBody_from_LLVMBuildIRBody(fn);

  fn->add_resource(std::move(vtree), "Virtual Node Tree");
  fn->add_resource(std::move(data_graph), "VTreeDataGraph");
  return fn;
}

}  // namespace DataFlowNodes
}  // namespace FN
