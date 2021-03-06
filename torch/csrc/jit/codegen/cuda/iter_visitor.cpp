#include <torch/csrc/jit/codegen/cuda/iter_visitor.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/type.h>

namespace torch {
namespace jit {
namespace fuser {

std::vector<Statement*> IterVisitor::next(Statement* statement) {
  if (statement->isVal())
    return next(static_cast<Val*>(statement));
  else if (statement->isExpr())
    return next(static_cast<Expr*>(statement));
  else
    TORCH_INTERNAL_ASSERT(
        false, "IterVisitor could not detect type in next_dispatch.");
}

std::vector<Statement*> IterVisitor::next(Val* v) {
  if (FusionGuard::getCurFusion()->origin(v) != nullptr)
    return {FusionGuard::getCurFusion()->origin(v)};
  return {};
}

std::vector<Statement*> IterVisitor::next(Expr* expr) {
  FusionGuard::getCurFusion()->assertInFusion(expr, "Cannot traverse expr, ");
  return {expr->inputs().begin(), expr->inputs().end()};
}

// Remove any stmt in stmts that is in visited
namespace {
void remove_visited(
    std::vector<Statement*>& stmts,
    const std::unordered_set<Statement*>& visited) {
  std::deque<std::vector<Statement*>::iterator> to_erase;
  for (auto it = stmts.begin(); it != stmts.end(); it++) {
    if (visited.find(*it) != visited.end())
      to_erase.push_back(it);
  }

  while (!to_erase.empty()) {
    stmts.erase(to_erase.back());
    to_erase.pop_back();
  }
}
} // namespace

void IterVisitor::traverseFrom(
    Fusion* const fusion,
    const std::vector<Val*>& from,
    bool traverseAllPaths) {
  FusionGuard fg(fusion);
  std::unordered_set<Statement*> visited;
  stmt_stack.clear();
  stmt_stack.emplace_back(from.rbegin(), from.rend());

  while (!stmt_stack.empty()) {
    auto next_stmts = next(stmt_stack.back().back());

    // Remove statements we already visited if we're not traversing all paths
    if (!traverseAllPaths)
      remove_visited(next_stmts, visited);

    // Traverse down until we get to a leaf
    while (!next_stmts.empty()) {
      stmt_stack.emplace_back(next_stmts.rbegin(), next_stmts.rend());
      next_stmts = next(stmt_stack.back().back());
      // Remove statements we already visited if we're not traversing all paths
      if (!traverseAllPaths)
        remove_visited(next_stmts, visited);
    }

    // Traverse back up
    // Mark visited
    visited.emplace(stmt_stack.back().back());
    // Handle
    handle(stmt_stack.back().back());
    // Remove
    stmt_stack.back().pop_back();

    while (!stmt_stack.empty() && stmt_stack.back().empty()) {
      stmt_stack.pop_back();
      if (!stmt_stack.empty()) {
        // Mark visited
        visited.emplace(stmt_stack.back().back());
        // Handle
        handle(stmt_stack.back().back());
        // Remove
        stmt_stack.back().pop_back();
      }
    }
  }
}

void IterVisitor::traverse_(
    Fusion* const fusion,
    bool from_outputs_only,
    bool breadth_first,
    bool traverse_all_paths) {
  FusionGuard fg(fusion);
  if (breadth_first)
    TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");

  if (from_outputs_only) {
    auto term_outs = DependencyCheck::getTerminatingOutputs(fusion);
    if (!term_outs.empty())
      traverseFrom(fusion, term_outs, traverse_all_paths);
    return;
  }

  std::vector<Val*> leaves;
  // Search for Vals with no uses (output edges)
  for (Val* val : fusion->deterministic_vals())
    if (!fusion->used(val))
      leaves.push_back(val);

  if (!leaves.empty())
    traverseFrom(fusion, leaves, traverse_all_paths);
}

void IterVisitor::traverse(
    Fusion* const fusion,
    bool from_outputs_only,
    bool breadth_first) {
  traverse_(fusion, from_outputs_only, breadth_first, false);
}

void IterVisitor::traverseAllPaths(
    Fusion* const fusion,
    bool from_outputs_only,
    bool breadth_first) {
  traverse_(fusion, from_outputs_only, breadth_first, true);
}

/* DEPENDENCY CHECKING */

namespace {

// Looks for and returns
struct DependencyChains : public IterVisitor {
  std::deque<std::deque<Val*>> dep_chains;
  bool is_dependency = false;
  std::set<Val*> dependencies_;

  void handle(Val* val) override {
    if (dependencies_.find(val) != dependencies_.end()) {
      is_dependency = true;
      std::deque<Val*> deps;
      for (auto stack : stmt_stack) {
        if (stack.back()->isVal())
          deps.push_back(static_cast<Val*>(stack.back()));
      }
      // Order as dependency -> of
      dep_chains.emplace_back(deps.rbegin(), deps.rend());
    }
  }

  DependencyChains(Val* _dependency, Val* _of, bool all_chains_ = false)
      : dependencies_({_dependency}) {
    traverseFrom(_of->fusion(), {_of}, all_chains_);
  }

  DependencyChains(Val* _dependency, bool all_chains_ = false)
      : dependencies_({_dependency}) {
    if (all_chains_)
      traverseAllPaths(_dependency->fusion(), false);
    else
      traverse(_dependency->fusion(), false);
  }

  DependencyChains(std::set<Val*> _dependencies, bool all_chains_ = false)
      : dependencies_(std::move(_dependencies)) {
    if (dependencies_.empty())
      return;

    if (all_chains_)
      traverseAllPaths((*dependencies_.begin())->fusion(), false);
    else
      traverse((*dependencies_.begin())->fusion(), false);
  }

  static std::deque<Val*> getDependencyChain(Val* dependency, Val* of) {
    DependencyChains dp(dependency, of, false);
    if (dp.dep_chains.empty())
      return std::deque<Val*>();
    return dp.dep_chains[0];
  }

  static std::deque<std::deque<Val*>> getDependencyChains(
      Val* dependency,
      Val* of) {
    DependencyChains dp(dependency, of, true);
    if (dp.dep_chains.empty())
      return std::deque<std::deque<Val*>>();
    return dp.dep_chains;
  }

  static std::deque<std::deque<Val*>> getDependencyChainsTo(Val* dependency) {
    DependencyChains dp(dependency, true);
    if (dp.dep_chains.empty())
      return std::deque<std::deque<Val*>>();
    return dp.dep_chains;
  }

  static std::deque<std::deque<Val*>> getDependencyChainsTo(
      const std::set<Val*>& dependencies) {
    DependencyChains dp(dependencies, true);
    if (dp.dep_chains.empty())
      return std::deque<std::deque<Val*>>();
    return dp.dep_chains;
  }
};

// Expr sort will take a fusion and return a topologically sorted list of
// expressions.
struct Exprs : public IterVisitor {
 private:
  std::vector<Expr*> exprs;

  void handle(Expr* expr) override {
    exprs.push_back(expr);
  }

 public:
  static std::vector<Expr*> getExprs(
      Fusion* fusion,
      const std::vector<Val*>& from) {
    Exprs ex;
    ex.traverseFrom(fusion, from, false);
    return ex.exprs;
  }
};

} // namespace

bool DependencyCheck::isDependencyOf(Val* dependency, Val* of) {
  return !DependencyChains::getDependencyChain(dependency, of).empty();
}

std::deque<Val*> DependencyCheck::getSingleDependencyChain(
    Val* dependency,
    Val* of) {
  return DependencyChains::getDependencyChain(dependency, of);
}

std::deque<std::deque<Val*>> DependencyCheck::getAllDependencyChains(
    Val* dependency,
    Val* of) {
  return DependencyChains::getDependencyChains(dependency, of);
}

std::deque<std::deque<Val*>> DependencyCheck::getAllDependencyChainsTo(
    Val* dependency) {
  return DependencyChains::getDependencyChainsTo(dependency);
}

std::vector<Val*> DependencyCheck::getTerminatingOutputs(Fusion* const fusion) {
  FusionGuard fg(fusion);

  std::set<Val*> used_vals;
  for (auto expr : Exprs::getExprs(
           fusion,
           std::vector<Val*>(
               fusion->outputs().begin(), fusion->outputs().end()))) {
    for (auto inp : expr->inputs())
      used_vals.emplace(inp);
  }

  std::vector<Val*> terminating_outputs;
  for (auto out : fusion->outputs())
    if (used_vals.find(out) == used_vals.end())
      terminating_outputs.push_back(out);

  return terminating_outputs;
}

} // namespace fuser
} // namespace jit
} // namespace torch
