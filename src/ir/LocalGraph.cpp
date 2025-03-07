/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iterator>

#include <cfg/cfg-traversal.h>
#include <ir/find_all.h>
#include <ir/local-graph.h>
#include <wasm-builder.h>

namespace wasm {

namespace LocalGraphInternal {

// Information about a basic block.
struct Info {
  // actions occurring in this block: local.gets and local.sets
  std::vector<Expression*> actions;
  // for each index, the last local.set for it
  std::unordered_map<Index, LocalSet*> lastSets;
};

// flow helper class. flows the gets to their sets

struct Flower : public CFGWalker<Flower, Visitor<Flower>, Info> {
  LocalGraph::GetSetses& getSetses;
  LocalGraph::Locations& locations;

  Flower(LocalGraph::GetSetses& getSetses,
         LocalGraph::Locations& locations,
         Function* func,
         Module* module)
    : getSetses(getSetses), locations(locations) {
    setFunction(func);
    setModule(module);
    // create the CFG by walking the IR
    CFGWalker<Flower, Visitor<Flower>, Info>::doWalkFunction(func);
    // flow gets across blocks
    flow(func);
  }

  BasicBlock* makeBasicBlock() { return new BasicBlock(); }

  // Branches outside of the function can be ignored, as we only look at locals
  // which vanish when we leave.
  bool ignoreBranchesOutsideOfFunc = true;

  // cfg traversal work

  static void doVisitLocalGet(Flower* self, Expression** currp) {
    auto* curr = (*currp)->cast<LocalGet>();
    // if in unreachable code, skip
    if (!self->currBasicBlock) {
      return;
    }
    self->currBasicBlock->contents.actions.emplace_back(curr);
    self->locations[curr] = currp;
  }

  static void doVisitLocalSet(Flower* self, Expression** currp) {
    auto* curr = (*currp)->cast<LocalSet>();
    // if in unreachable code, skip
    if (!self->currBasicBlock) {
      return;
    }
    self->currBasicBlock->contents.actions.emplace_back(curr);
    self->currBasicBlock->contents.lastSets[curr->index] = curr;
    self->locations[curr] = currp;
  }

  void flow(Function* func) {
    // This block struct is optimized for this flow process (Minimal
    // information, iteration index).
    struct FlowBlock {
      // Last Traversed Iteration: This value helps us to find if this block has
      // been seen while traversing blocks. We compare this value to the current
      // iteration index in order to determine if we already process this block
      // in the current iteration. This speeds up the processing compared to
      // unordered_set or other struct usage. (No need to reset internal values,
      // lookup into container, ...)
      size_t lastTraversedIteration;
      std::vector<Expression*> actions;
      std::vector<FlowBlock*> in;
      // Sor each index, the last local.set for it
      // The unordered_map from BasicBlock.Info is converted into a vector
      // This speeds up search as there are usually few sets in a block, so just
      // scanning them linearly is efficient, avoiding hash computations (while
      // in Info, it's convenient to have a map so we can assign them easily,
      // where the last one seen overwrites the previous; and, we do that O(1)).
      std::vector<std::pair<Index, LocalSet*>> lastSets;
    };

    auto numLocals = func->getNumLocals();
    std::vector<std::vector<LocalGet*>> allGets;
    allGets.resize(numLocals);
    std::vector<FlowBlock*> work;

    // Convert input blocks (basicBlocks) into more efficient flow blocks to
    // improve memory access.
    std::vector<FlowBlock> flowBlocks;
    flowBlocks.resize(basicBlocks.size());

    // Init mapping between basicblocks and flowBlocks
    std::unordered_map<BasicBlock*, FlowBlock*> basicToFlowMap;
    for (Index i = 0; i < basicBlocks.size(); ++i) {
      basicToFlowMap[basicBlocks[i].get()] = &flowBlocks[i];
    }

    const size_t NULL_ITERATION = -1;

    FlowBlock* entryFlowBlock = nullptr;
    for (Index i = 0; i < flowBlocks.size(); ++i) {
      auto& block = basicBlocks[i];
      auto& flowBlock = flowBlocks[i];
      // Get the equivalent block to entry in the flow list
      if (block.get() == entry) {
        entryFlowBlock = &flowBlock;
      }
      flowBlock.lastTraversedIteration = NULL_ITERATION;
      flowBlock.actions.swap(block->contents.actions);
      // Map in block to flow blocks
      auto& in = block->in;
      flowBlock.in.resize(in.size());
      std::transform(in.begin(),
                     in.end(),
                     flowBlock.in.begin(),
                     [&](BasicBlock* block) { return basicToFlowMap[block]; });
      // Convert unordered_map to vector.
      flowBlock.lastSets.reserve(block->contents.lastSets.size());
      for (auto set : block->contents.lastSets) {
        flowBlock.lastSets.emplace_back(set);
      }
    }
    assert(entryFlowBlock != nullptr);

    size_t currentIteration = 0;
    for (auto& block : flowBlocks) {
#ifdef LOCAL_GRAPH_DEBUG
      std::cout << "basic block " << &block << " :\n";
      for (auto& action : block.actions) {
        std::cout << "  action: " << *action << '\n';
      }
      for (auto& val : block.lastSets) {
        std::cout << "  last set " << val.second << '\n';
      }
#endif
      // go through the block, finding each get and adding it to its index,
      // and seeing how sets affect that
      auto& actions = block.actions;
      // move towards the front, handling things as we go
      for (int i = int(actions.size()) - 1; i >= 0; i--) {
        auto* action = actions[i];
        if (auto* get = action->dynCast<LocalGet>()) {
          allGets[get->index].push_back(get);
        } else {
          // This set is the only set for all those gets.
          auto* set = action->cast<LocalSet>();
          auto& gets = allGets[set->index];
          for (auto* get : gets) {
            getSetses[get].insert(set);
          }
          gets.clear();
        }
      }
      // If anything is left, we must flow it back through other blocks. we
      // can do that for all gets as a whole, they will get the same results.
      for (Index index = 0; index < numLocals; index++) {
        auto& gets = allGets[index];
        if (gets.empty()) {
          continue;
        }
        work.push_back(&block);
        // Note that we may need to revisit the later parts of this initial
        // block, if we are in a loop, so don't mark it as seen.
        while (!work.empty()) {
          auto* curr = work.back();
          work.pop_back();
          // We have gone through this block; now we must handle flowing to
          // the inputs.
          if (curr->in.empty()) {
            if (curr == entryFlowBlock) {
              // These receive a param or zero init value.
              for (auto* get : gets) {
                getSetses[get].insert(nullptr);
              }
            }
          } else {
            for (auto* pred : curr->in) {
              if (pred->lastTraversedIteration == currentIteration) {
                // We've already seen pred in this iteration.
                continue;
              }
              pred->lastTraversedIteration = currentIteration;
              auto lastSet =
                std::find_if(pred->lastSets.begin(),
                             pred->lastSets.end(),
                             [&](std::pair<Index, LocalSet*>& value) {
                               return value.first == index;
                             });
              if (lastSet != pred->lastSets.end()) {
                // There is a set here, apply it, and stop the flow.
                for (auto* get : gets) {
                  getSetses[get].insert(lastSet->second);
                }
              } else {
                // Keep on flowing.
                work.push_back(pred);
              }
            }
          }
        }
        gets.clear();
        currentIteration++;
      }
    }
  }
};

} // namespace LocalGraphInternal

// LocalGraph implementation

LocalGraph::LocalGraph(Function* func, Module* module) : func(func) {
  LocalGraphInternal::Flower flower(getSetses, locations, func, module);

#ifdef LOCAL_GRAPH_DEBUG
  std::cout << "LocalGraph::dump\n";
  for (auto& [get, sets] : getSetses) {
    std::cout << "GET\n" << get << " is influenced by\n";
    for (auto* set : sets) {
      std::cout << set << '\n';
    }
  }
  std::cout << "total locations: " << locations.size() << '\n';
#endif
}

bool LocalGraph::equivalent(LocalGet* a, LocalGet* b) {
  auto& aSets = getSetses[a];
  auto& bSets = getSetses[b];
  // The simple case of one set dominating two gets easily proves that they must
  // have the same value. (Note that we can infer dominance from the fact that
  // there is a single set: if the set did not dominate one of the gets then
  // there would definitely be another set for that get, the zero initialization
  // at the function entry, if nothing else.)
  if (aSets.size() != 1 || bSets.size() != 1) {
    // TODO: use a LinearExecutionWalker to find trivially equal gets in basic
    //       blocks. that plus the above should handle 80% of cases.
    // TODO: handle chains, merges and other situations
    return false;
  }
  auto* aSet = *aSets.begin();
  auto* bSet = *bSets.begin();
  if (aSet != bSet) {
    return false;
  }
  if (!aSet) {
    // They are both nullptr, indicating the implicit value for a parameter
    // or the zero for a local.
    if (func->isParam(a->index)) {
      // For parameters to be equivalent they must have the exact same
      // index.
      return a->index == b->index;
    } else {
      // As locals, they are both of value zero, but must have the right
      // type as well.
      return func->getLocalType(a->index) == func->getLocalType(b->index);
    }
  } else {
    // They are both the same actual set.
    return true;
  }
}

void LocalGraph::computeSetInfluences() {
  for (auto& [curr, _] : locations) {
    if (auto* get = curr->dynCast<LocalGet>()) {
      for (auto* set : getSetses[get]) {
        setInfluences[set].insert(get);
      }
    }
  }
}

void LocalGraph::computeGetInfluences() {
  for (auto& [curr, _] : locations) {
    if (auto* set = curr->dynCast<LocalSet>()) {
      FindAll<LocalGet> findAll(set->value);
      for (auto* get : findAll.list) {
        getInfluences[get].insert(set);
      }
    }
  }
}

void LocalGraph::computeSSAIndexes() {
  std::unordered_map<Index, std::set<LocalSet*>> indexSets;
  for (auto& [get, sets] : getSetses) {
    for (auto* set : sets) {
      indexSets[get->index].insert(set);
    }
  }
  for (auto& [curr, _] : locations) {
    if (auto* set = curr->dynCast<LocalSet>()) {
      auto& sets = indexSets[set->index];
      if (sets.size() == 1 && *sets.begin() != curr) {
        // While it has just one set, it is not the right one (us),
        // so mark it invalid.
        sets.clear();
      }
    }
  }
  for (auto& [index, sets] : indexSets) {
    if (sets.size() == 1) {
      SSAIndexes.insert(index);
    }
  }
}

bool LocalGraph::isSSA(Index x) { return SSAIndexes.count(x); }

} // namespace wasm
