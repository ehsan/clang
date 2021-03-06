//==- UninitializedValues.cpp - Find Uninitialized Values -------*- C++ --*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements uninitialized values analysis for source-level CFGs.
//
//===----------------------------------------------------------------------===//

#include <utility>
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PackedVector.h"
#include "llvm/ADT/DenseMap.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/AnalysisContext.h"
#include "clang/Analysis/Visitors/CFGRecStmtDeclVisitor.h"
#include "clang/Analysis/Analyses/UninitializedValues.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace clang;

static bool isTrackedVar(const VarDecl *vd, const DeclContext *dc) {
  if (vd->isLocalVarDecl() && !vd->hasGlobalStorage() &&
      !vd->isExceptionVariable() &&
      vd->getDeclContext() == dc) {
    QualType ty = vd->getType();
    return ty->isScalarType() || ty->isVectorType();
  }
  return false;
}

//------------------------------------------------------------------------====//
// DeclToIndex: a mapping from Decls we track to value indices.
//====------------------------------------------------------------------------//

namespace {
class DeclToIndex {
  llvm::DenseMap<const VarDecl *, unsigned> map;
public:
  DeclToIndex() {}
  
  /// Compute the actual mapping from declarations to bits.
  void computeMap(const DeclContext &dc);
  
  /// Return the number of declarations in the map.
  unsigned size() const { return map.size(); }
  
  /// Returns the bit vector index for a given declaration.
  llvm::Optional<unsigned> getValueIndex(const VarDecl *d) const;
};
}

void DeclToIndex::computeMap(const DeclContext &dc) {
  unsigned count = 0;
  DeclContext::specific_decl_iterator<VarDecl> I(dc.decls_begin()),
                                               E(dc.decls_end());
  for ( ; I != E; ++I) {
    const VarDecl *vd = *I;
    if (isTrackedVar(vd, &dc))
      map[vd] = count++;
  }
}

llvm::Optional<unsigned> DeclToIndex::getValueIndex(const VarDecl *d) const {
  llvm::DenseMap<const VarDecl *, unsigned>::const_iterator I = map.find(d);
  if (I == map.end())
    return llvm::Optional<unsigned>();
  return I->second;
}

//------------------------------------------------------------------------====//
// CFGBlockValues: dataflow values for CFG blocks.
//====------------------------------------------------------------------------//

// These values are defined in such a way that a merge can be done using
// a bitwise OR.
enum Value { Unknown = 0x0,         /* 00 */
             Initialized = 0x1,     /* 01 */
             Uninitialized = 0x2,   /* 10 */
             MayUninitialized = 0x3 /* 11 */ };

static bool isUninitialized(const Value v) {
  return v >= Uninitialized;
}
static bool isAlwaysUninit(const Value v) {
  return v == Uninitialized;
}

namespace {

typedef llvm::PackedVector<Value, 2> ValueVector;
typedef std::pair<ValueVector *, ValueVector *> BVPair;

class CFGBlockValues {
  const CFG &cfg;
  BVPair *vals;
  ValueVector scratch;
  DeclToIndex declToIndex;
  
  ValueVector &lazyCreate(ValueVector *&bv);
public:
  CFGBlockValues(const CFG &cfg);
  ~CFGBlockValues();
  
  unsigned getNumEntries() const { return declToIndex.size(); }
  
  void computeSetOfDeclarations(const DeclContext &dc);  
  ValueVector &getValueVector(const CFGBlock *block,
                              const CFGBlock *dstBlock);

  BVPair &getValueVectors(const CFGBlock *block, bool shouldLazyCreate);

  void mergeIntoScratch(ValueVector const &source, bool isFirst);
  bool updateValueVectorWithScratch(const CFGBlock *block);
  bool updateValueVectors(const CFGBlock *block, const BVPair &newVals);
  
  bool hasNoDeclarations() const {
    return declToIndex.size() == 0;
  }

  void resetScratch();
  ValueVector &getScratch() { return scratch; }
  
  ValueVector::reference operator[](const VarDecl *vd);

  Value getValue(const CFGBlock *block, const CFGBlock *dstBlock,
                 const VarDecl *vd) {
    const llvm::Optional<unsigned> &idx = declToIndex.getValueIndex(vd);
    assert(idx.hasValue());
    return getValueVector(block, dstBlock)[idx.getValue()];
  }
};  
} // end anonymous namespace

CFGBlockValues::CFGBlockValues(const CFG &c) : cfg(c), vals(0) {
  unsigned n = cfg.getNumBlockIDs();
  if (!n)
    return;
  vals = new std::pair<ValueVector*, ValueVector*>[n];
  memset((void*)vals, 0, sizeof(*vals) * n);
}

CFGBlockValues::~CFGBlockValues() {
  unsigned n = cfg.getNumBlockIDs();
  if (n == 0)
    return;
  for (unsigned i = 0; i < n; ++i) {
    delete vals[i].first;
    delete vals[i].second;
  }
  delete [] vals;
}

void CFGBlockValues::computeSetOfDeclarations(const DeclContext &dc) {
  declToIndex.computeMap(dc);
  scratch.resize(declToIndex.size());
}

ValueVector &CFGBlockValues::lazyCreate(ValueVector *&bv) {
  if (!bv)
    bv = new ValueVector(declToIndex.size());
  return *bv;
}

/// This function pattern matches for a '&&' or '||' that appears at
/// the beginning of a CFGBlock that also (1) has a terminator and 
/// (2) has no other elements.  If such an expression is found, it is returned.
static const BinaryOperator *getLogicalOperatorInChain(const CFGBlock *block) {
  if (block->empty())
    return 0;

  CFGElement front = block->front();
  const CFGStmt *cstmt = front.getAs<CFGStmt>();
  if (!cstmt)
    return 0;

  const BinaryOperator *b = dyn_cast_or_null<BinaryOperator>(cstmt->getStmt());
  
  if (!b || !b->isLogicalOp())
    return 0;
  
  if (block->pred_size() == 2) {
    if (block->getTerminatorCondition() == b) {
      if (block->succ_size() == 2)
      return b;
    }
    else if (block->size() == 1)
      return b;
  }

  return 0;
}

ValueVector &CFGBlockValues::getValueVector(const CFGBlock *block,
                                            const CFGBlock *dstBlock) {
  unsigned idx = block->getBlockID();
  if (dstBlock && getLogicalOperatorInChain(block)) {
    if (*block->succ_begin() == dstBlock)
      return lazyCreate(vals[idx].first);
    assert(*(block->succ_begin()+1) == dstBlock);
    return lazyCreate(vals[idx].second);
  }

  assert(vals[idx].second == 0);
  return lazyCreate(vals[idx].first);
}

BVPair &CFGBlockValues::getValueVectors(const clang::CFGBlock *block,
                                        bool shouldLazyCreate) {
  unsigned idx = block->getBlockID();
  lazyCreate(vals[idx].first);
  if (shouldLazyCreate)
    lazyCreate(vals[idx].second);
  return vals[idx];
}

#if 0
static void printVector(const CFGBlock *block, ValueVector &bv,
                        unsigned num) {
  
  llvm::errs() << block->getBlockID() << " :";
  for (unsigned i = 0; i < bv.size(); ++i) {
    llvm::errs() << ' ' << bv[i];
  }
  llvm::errs() << " : " << num << '\n';
}

static void printVector(const char *name, ValueVector const &bv) {
  llvm::errs() << name << " : ";
  for (unsigned i = 0; i < bv.size(); ++i) {
    llvm::errs() << ' ' << bv[i];
  }
  llvm::errs() << "\n";
}
#endif

void CFGBlockValues::mergeIntoScratch(ValueVector const &source,
                                      bool isFirst) {
  if (isFirst)
    scratch = source;
  else
    scratch |= source;
}

bool CFGBlockValues::updateValueVectorWithScratch(const CFGBlock *block) {
  ValueVector &dst = getValueVector(block, 0);
  bool changed = (dst != scratch);
  if (changed)
    dst = scratch;
#if 0
  printVector(block, scratch, 0);
#endif
  return changed;
}

bool CFGBlockValues::updateValueVectors(const CFGBlock *block,
                                      const BVPair &newVals) {
  BVPair &vals = getValueVectors(block, true);
  bool changed = *newVals.first != *vals.first ||
                 *newVals.second != *vals.second;
  *vals.first = *newVals.first;
  *vals.second = *newVals.second;
#if 0
  printVector(block, *vals.first, 1);
  printVector(block, *vals.second, 2);
#endif
  return changed;
}

void CFGBlockValues::resetScratch() {
  scratch.reset();
}

ValueVector::reference CFGBlockValues::operator[](const VarDecl *vd) {
  const llvm::Optional<unsigned> &idx = declToIndex.getValueIndex(vd);
  assert(idx.hasValue());
  return scratch[idx.getValue()];
}

//------------------------------------------------------------------------====//
// Worklist: worklist for dataflow analysis.
//====------------------------------------------------------------------------//

namespace {
class DataflowWorklist {
  SmallVector<const CFGBlock *, 20> worklist;
  llvm::BitVector enqueuedBlocks;
public:
  DataflowWorklist(const CFG &cfg) : enqueuedBlocks(cfg.getNumBlockIDs()) {}
  
  void enqueueSuccessors(const CFGBlock *block);
  const CFGBlock *dequeue();
};
}

void DataflowWorklist::enqueueSuccessors(const clang::CFGBlock *block) {
  unsigned OldWorklistSize = worklist.size();
  for (CFGBlock::const_succ_iterator I = block->succ_begin(),
       E = block->succ_end(); I != E; ++I) {
    const CFGBlock *Successor = *I;
    if (!Successor || enqueuedBlocks[Successor->getBlockID()])
      continue;
    worklist.push_back(Successor);
    enqueuedBlocks[Successor->getBlockID()] = true;
  }
  if (OldWorklistSize == 0 || OldWorklistSize == worklist.size())
    return;

  // Rotate the newly added blocks to the start of the worklist so that it forms
  // a proper queue when we pop off the end of the worklist.
  std::rotate(worklist.begin(), worklist.begin() + OldWorklistSize,
              worklist.end());
}

const CFGBlock *DataflowWorklist::dequeue() {
  if (worklist.empty())
    return 0;
  const CFGBlock *b = worklist.back();
  worklist.pop_back();
  enqueuedBlocks[b->getBlockID()] = false;
  return b;
}

//------------------------------------------------------------------------====//
// Transfer function for uninitialized values analysis.
//====------------------------------------------------------------------------//

namespace {
class FindVarResult {
  const VarDecl *vd;
  const DeclRefExpr *dr;
public:
  FindVarResult(VarDecl *vd, DeclRefExpr *dr) : vd(vd), dr(dr) {}
  
  const DeclRefExpr *getDeclRefExpr() const { return dr; }
  const VarDecl *getDecl() const { return vd; }
};
  
class TransferFunctions : public StmtVisitor<TransferFunctions> {
  CFGBlockValues &vals;
  const CFG &cfg;
  const CFGBlock *block;
  AnalysisDeclContext &ac;
  UninitVariablesHandler *handler;
  
  /// The last DeclRefExpr seen when analyzing a block.  Used to
  /// cheat when detecting cases when the address of a variable is taken.
  DeclRefExpr *lastDR;
  
  /// The last lvalue-to-rvalue conversion of a variable whose value
  /// was uninitialized.  Normally this results in a warning, but it is
  /// possible to either silence the warning in some cases, or we
  /// propagate the uninitialized value.
  CastExpr *lastLoad;
  
  /// For some expressions, we want to ignore any post-processing after
  /// visitation.
  bool skipProcessUses;
  
public:
  TransferFunctions(CFGBlockValues &vals, const CFG &cfg,
                    const CFGBlock *block, AnalysisDeclContext &ac,
                    UninitVariablesHandler *handler)
    : vals(vals), cfg(cfg), block(block), ac(ac), handler(handler),
      lastDR(0), lastLoad(0),
      skipProcessUses(false) {}
  
  void reportUse(const Expr *ex, const VarDecl *vd);

  void VisitBlockExpr(BlockExpr *be);
  void VisitDeclStmt(DeclStmt *ds);
  void VisitDeclRefExpr(DeclRefExpr *dr);
  void VisitUnaryOperator(UnaryOperator *uo);
  void VisitBinaryOperator(BinaryOperator *bo);
  void VisitCastExpr(CastExpr *ce);
  void VisitObjCForCollectionStmt(ObjCForCollectionStmt *fs);
  void Visit(Stmt *s);

  bool isTrackedVar(const VarDecl *vd) {
    return ::isTrackedVar(vd, cast<DeclContext>(ac.getDecl()));
  }

  UninitUse getUninitUse(const Expr *ex, const VarDecl *vd, Value v) {
    UninitUse Use(ex, isAlwaysUninit(v));

    assert(isUninitialized(v));
    if (Use.getKind() == UninitUse::Always)
      return Use;

    // If an edge which leads unconditionally to this use did not initialize
    // the variable, we can say something stronger than 'may be uninitialized':
    // we can say 'either it's used uninitialized or you have dead code'.
    //
    // We track the number of successors of a node which have been visited, and
    // visit a node once we have visited all of its successors. Only edges where
    // the variable might still be uninitialized are followed. Since a variable
    // can't transfer from being initialized to being uninitialized, this will
    // trace out the subgraph which inevitably leads to the use and does not
    // initialize the variable. We do not want to skip past loops, since their
    // non-termination might be correlated with the initialization condition.
    //
    // For example:
    //
    //         void f(bool a, bool b) {
    // block1:   int n;
    //           if (a) {
    // block2:     if (b)
    // block3:       n = 1;
    // block4:   } else if (b) {
    // block5:     while (!a) {
    // block6:       do_work(&a);
    //               n = 2;
    //             }
    //           }
    // block7:   if (a)
    // block8:     g();
    // block9:   return n;
    //         }
    //
    // Starting from the maybe-uninitialized use in block 9:
    //  * Block 7 is not visited because we have only visited one of its two
    //    successors.
    //  * Block 8 is visited because we've visited its only successor.
    // From block 8:
    //  * Block 7 is visited because we've now visited both of its successors.
    // From block 7:
    //  * Blocks 1, 2, 4, 5, and 6 are not visited because we didn't visit all
    //    of their successors (we didn't visit 4, 3, 5, 6, and 5, respectively).
    //  * Block 3 is not visited because it initializes 'n'.
    // Now the algorithm terminates, having visited blocks 7 and 8, and having
    // found the frontier is blocks 2, 4, and 5.
    //
    // 'n' is definitely uninitialized for two edges into block 7 (from blocks 2
    // and 4), so we report that any time either of those edges is taken (in
    // each case when 'b == false'), 'n' is used uninitialized.
    llvm::SmallVector<const CFGBlock*, 32> Queue;
    llvm::SmallVector<unsigned, 32> SuccsVisited(cfg.getNumBlockIDs(), 0);
    Queue.push_back(block);
    // Specify that we've already visited all successors of the starting block.
    // This has the dual purpose of ensuring we never add it to the queue, and
    // of marking it as not being a candidate element of the frontier.
    SuccsVisited[block->getBlockID()] = block->succ_size();
    while (!Queue.empty()) {
      const CFGBlock *B = Queue.back();
      Queue.pop_back();
      for (CFGBlock::const_pred_iterator I = B->pred_begin(), E = B->pred_end();
           I != E; ++I) {
        const CFGBlock *Pred = *I;
        if (vals.getValue(Pred, B, vd) == Initialized)
          // This block initializes the variable.
          continue;

        if (++SuccsVisited[Pred->getBlockID()] == Pred->succ_size())
          // All paths from this block lead to the use and don't initialize the
          // variable.
          Queue.push_back(Pred);
      }
    }

    // Scan the frontier, looking for blocks where the variable was
    // uninitialized.
    for (CFG::const_iterator BI = cfg.begin(), BE = cfg.end(); BI != BE; ++BI) {
      const CFGBlock *Block = *BI;
      unsigned BlockID = Block->getBlockID();
      const Stmt *Term = Block->getTerminator();
      if (SuccsVisited[BlockID] && SuccsVisited[BlockID] < Block->succ_size() &&
          Term) {
        // This block inevitably leads to the use. If we have an edge from here
        // to a post-dominator block, and the variable is uninitialized on that
        // edge, we have found a bug.
        for (CFGBlock::const_succ_iterator I = Block->succ_begin(),
             E = Block->succ_end(); I != E; ++I) {
          const CFGBlock *Succ = *I;
          if (Succ && SuccsVisited[Succ->getBlockID()] >= Succ->succ_size() &&
              vals.getValue(Block, Succ, vd) == Uninitialized) {
            // Switch cases are a special case: report the label to the caller
            // as the 'terminator', not the switch statement itself. Suppress
            // situations where no label matched: we can't be sure that's
            // possible.
            if (isa<SwitchStmt>(Term)) {
              const Stmt *Label = Succ->getLabel();
              if (!Label || !isa<SwitchCase>(Label))
                // Might not be possible.
                continue;
              UninitUse::Branch Branch;
              Branch.Terminator = Label;
              Branch.Output = 0; // Ignored.
              Use.addUninitBranch(Branch);
            } else {
              UninitUse::Branch Branch;
              Branch.Terminator = Term;
              Branch.Output = I - Block->succ_begin();
              Use.addUninitBranch(Branch);
            }
          }
        }
      }
    }

    return Use;
  }

  FindVarResult findBlockVarDecl(Expr *ex);
  
  void ProcessUses(Stmt *s = 0);
};
}

static const Expr *stripCasts(ASTContext &C, const Expr *Ex) {
  while (Ex) {
    Ex = Ex->IgnoreParenNoopCasts(C);
    if (const CastExpr *CE = dyn_cast<CastExpr>(Ex)) {
      if (CE->getCastKind() == CK_LValueBitCast) {
        Ex = CE->getSubExpr();
        continue;
      }
    }
    break;
  }
  return Ex;
}

void TransferFunctions::reportUse(const Expr *ex, const VarDecl *vd) {
  if (!handler)
    return;
  Value v = vals[vd];
  if (isUninitialized(v))
    handler->handleUseOfUninitVariable(vd, getUninitUse(ex, vd, v));
}

FindVarResult TransferFunctions::findBlockVarDecl(Expr *ex) {
  if (DeclRefExpr *dr = dyn_cast<DeclRefExpr>(ex->IgnoreParenCasts()))
    if (VarDecl *vd = dyn_cast<VarDecl>(dr->getDecl()))
      if (isTrackedVar(vd))
        return FindVarResult(vd, dr);  
  return FindVarResult(0, 0);
}

void TransferFunctions::VisitObjCForCollectionStmt(ObjCForCollectionStmt *fs) {
  // This represents an initialization of the 'element' value.
  Stmt *element = fs->getElement();
  const VarDecl *vd = 0;
  
  if (DeclStmt *ds = dyn_cast<DeclStmt>(element)) {
    vd = cast<VarDecl>(ds->getSingleDecl());
    if (!isTrackedVar(vd))
      vd = 0;
  } else {
    // Initialize the value of the reference variable.
    const FindVarResult &res = findBlockVarDecl(cast<Expr>(element));
    vd = res.getDecl();
  }
  
  if (vd)
    vals[vd] = Initialized;
}

void TransferFunctions::VisitBlockExpr(BlockExpr *be) {
  const BlockDecl *bd = be->getBlockDecl();
  for (BlockDecl::capture_const_iterator i = bd->capture_begin(),
        e = bd->capture_end() ; i != e; ++i) {
    const VarDecl *vd = i->getVariable();
    if (!isTrackedVar(vd))
      continue;
    if (i->isByRef()) {
      vals[vd] = Initialized;
      continue;
    }
    reportUse(be, vd);
  }
}

void TransferFunctions::VisitDeclRefExpr(DeclRefExpr *dr) {
  // Record the last DeclRefExpr seen.  This is an lvalue computation.
  // We use this value to later detect if a variable "escapes" the analysis.
  if (const VarDecl *vd = dyn_cast<VarDecl>(dr->getDecl()))
    if (isTrackedVar(vd)) {
      ProcessUses();
      lastDR = dr;
    }
}

void TransferFunctions::VisitDeclStmt(DeclStmt *ds) {
  for (DeclStmt::decl_iterator DI = ds->decl_begin(), DE = ds->decl_end();
       DI != DE; ++DI) {
    if (VarDecl *vd = dyn_cast<VarDecl>(*DI)) {
      if (isTrackedVar(vd)) {
        if (Expr *init = vd->getInit()) {
          // If the initializer consists solely of a reference to itself, we
          // explicitly mark the variable as uninitialized. This allows code
          // like the following:
          //
          //   int x = x;
          //
          // to deliberately leave a variable uninitialized. Different analysis
          // clients can detect this pattern and adjust their reporting
          // appropriately, but we need to continue to analyze subsequent uses
          // of the variable.
          if (init == lastLoad) {
            const DeclRefExpr *DR
              = cast<DeclRefExpr>(stripCasts(ac.getASTContext(),
                                             lastLoad->getSubExpr()));
            if (DR->getDecl() == vd) {
              // int x = x;
              // Propagate uninitialized value, but don't immediately report
              // a problem.
              vals[vd] = Uninitialized;
              lastLoad = 0;
              lastDR = 0;
              if (handler)
                handler->handleSelfInit(vd);
              return;
            }
          }

          // All other cases: treat the new variable as initialized.
          // This is a minor optimization to reduce the propagation
          // of the analysis, since we will have already reported
          // the use of the uninitialized value (which visiting the
          // initializer).
          vals[vd] = Initialized;
        } else {
          // No initializer: the variable is now uninitialized. This matters
          // for cases like:
          //   while (...) {
          //     int n;
          //     use(n);
          //     n = 0;
          //   }
          // FIXME: Mark the variable as uninitialized whenever its scope is
          // left, since its scope could be re-entered by a jump over the
          // declaration.
          vals[vd] = Uninitialized;
        }
      }
    }
  }
}

void TransferFunctions::VisitBinaryOperator(clang::BinaryOperator *bo) {
  if (bo->isAssignmentOp()) {
    const FindVarResult &res = findBlockVarDecl(bo->getLHS());
    if (const VarDecl *vd = res.getDecl()) {
      if (bo->getOpcode() != BO_Assign)
        reportUse(res.getDeclRefExpr(), vd);
      else
        vals[vd] = Initialized;
    }
  }
}

void TransferFunctions::VisitUnaryOperator(clang::UnaryOperator *uo) {
  switch (uo->getOpcode()) {
    case clang::UO_PostDec:
    case clang::UO_PostInc:
    case clang::UO_PreDec:
    case clang::UO_PreInc: {
      const FindVarResult &res = findBlockVarDecl(uo->getSubExpr());
      if (const VarDecl *vd = res.getDecl()) {
        assert(res.getDeclRefExpr() == lastDR);
        // We null out lastDR to indicate we have fully processed it
        // and we don't want the auto-value setting in Visit().
        lastDR = 0;
        reportUse(res.getDeclRefExpr(), vd);
      }
      break;
    }
    default:
      break;
  }
}

void TransferFunctions::VisitCastExpr(clang::CastExpr *ce) {
  if (ce->getCastKind() == CK_LValueToRValue) {
    const FindVarResult &res = findBlockVarDecl(ce->getSubExpr());
    if (res.getDecl()) {
      assert(res.getDeclRefExpr() == lastDR);
      lastLoad = ce;
    }
  }
  else if (ce->getCastKind() == CK_NoOp ||
           ce->getCastKind() == CK_LValueBitCast) {
    skipProcessUses = true;
  }
  else if (CStyleCastExpr *cse = dyn_cast<CStyleCastExpr>(ce)) {
    if (cse->getType()->isVoidType()) {
      // e.g. (void) x;
      if (lastLoad == cse->getSubExpr()) {
        // Squelch any detected load of an uninitialized value if
        // we cast it to void.
        lastLoad = 0;
        lastDR = 0;
      }
    }
  }
}

void TransferFunctions::Visit(clang::Stmt *s) {
  skipProcessUses = false;
  StmtVisitor<TransferFunctions>::Visit(s);
  if (!skipProcessUses)
    ProcessUses(s);
}

void TransferFunctions::ProcessUses(Stmt *s) {
  // This method is typically called after visiting a CFGElement statement
  // in the CFG.  We delay processing of reporting many loads of uninitialized
  // values until here.
  if (lastLoad) {
    // If we just visited the lvalue-to-rvalue cast, there is nothing
    // left to do.
    if (lastLoad == s)
      return;

    const DeclRefExpr *DR =
      cast<DeclRefExpr>(stripCasts(ac.getASTContext(),
                                   lastLoad->getSubExpr()));
    const VarDecl *VD = cast<VarDecl>(DR->getDecl());

    // If we reach here, we may have seen a load of an uninitialized value
    // and it hasn't been casted to void or otherwise handled.  In this
    // situation, report the incident.
    reportUse(DR, VD);

    lastLoad = 0;

    if (DR == lastDR) {
      lastDR = 0;
      return;
    }
  }

  // Any other uses of 'lastDR' involve taking an lvalue of variable.
  // In this case, it "escapes" the analysis.
  if (lastDR && lastDR != s) {
    vals[cast<VarDecl>(lastDR->getDecl())] = Initialized;
    lastDR = 0;
  }
}

//------------------------------------------------------------------------====//
// High-level "driver" logic for uninitialized values analysis.
//====------------------------------------------------------------------------//

static bool runOnBlock(const CFGBlock *block, const CFG &cfg,
                       AnalysisDeclContext &ac, CFGBlockValues &vals,
                       llvm::BitVector &wasAnalyzed,
                       UninitVariablesHandler *handler = 0) {
  
  wasAnalyzed[block->getBlockID()] = true;
  
  if (const BinaryOperator *b = getLogicalOperatorInChain(block)) {
    CFGBlock::const_pred_iterator itr = block->pred_begin();
    BVPair vA = vals.getValueVectors(*itr, false);
    ++itr;
    BVPair vB = vals.getValueVectors(*itr, false);

    BVPair valsAB;
    
    if (b->getOpcode() == BO_LAnd) {
      // Merge the 'F' bits from the first and second.
      vals.mergeIntoScratch(*(vA.second ? vA.second : vA.first), true);
      vals.mergeIntoScratch(*(vB.second ? vB.second : vB.first), false);
      valsAB.first = vA.first;
      valsAB.second = &vals.getScratch();
    } else {
      // Merge the 'T' bits from the first and second.
      assert(b->getOpcode() == BO_LOr);
      vals.mergeIntoScratch(*vA.first, true);
      vals.mergeIntoScratch(*vB.first, false);
      valsAB.first = &vals.getScratch();
      valsAB.second = vA.second ? vA.second : vA.first;
    }
    return vals.updateValueVectors(block, valsAB);
  }

  // Default behavior: merge in values of predecessor blocks.
  vals.resetScratch();
  bool isFirst = true;
  for (CFGBlock::const_pred_iterator I = block->pred_begin(),
       E = block->pred_end(); I != E; ++I) {
    const CFGBlock *pred = *I;
    if (wasAnalyzed[pred->getBlockID()]) {
      vals.mergeIntoScratch(vals.getValueVector(pred, block), isFirst);
      isFirst = false;
    }
  }
  // Apply the transfer function.
  TransferFunctions tf(vals, cfg, block, ac, handler);
  for (CFGBlock::const_iterator I = block->begin(), E = block->end(); 
       I != E; ++I) {
    if (const CFGStmt *cs = dyn_cast<CFGStmt>(&*I)) {
      tf.Visit(const_cast<Stmt*>(cs->getStmt()));
    }
  }
  tf.ProcessUses();
  return vals.updateValueVectorWithScratch(block);
}

void clang::runUninitializedVariablesAnalysis(
    const DeclContext &dc,
    const CFG &cfg,
    AnalysisDeclContext &ac,
    UninitVariablesHandler &handler,
    UninitVariablesAnalysisStats &stats) {
  CFGBlockValues vals(cfg);
  vals.computeSetOfDeclarations(dc);
  if (vals.hasNoDeclarations())
    return;
#if 0
  cfg.dump(dc.getParentASTContext().getLangOpts(), true);
#endif

  stats.NumVariablesAnalyzed = vals.getNumEntries();

  // Mark all variables uninitialized at the entry.
  const CFGBlock &entry = cfg.getEntry();
  for (CFGBlock::const_succ_iterator i = entry.succ_begin(), 
        e = entry.succ_end(); i != e; ++i) {
    if (const CFGBlock *succ = *i) {
      ValueVector &vec = vals.getValueVector(&entry, succ);
      const unsigned n = vals.getNumEntries();
      for (unsigned j = 0; j < n ; ++j) {
        vec[j] = Uninitialized;
      }
    }
  }

  // Proceed with the workist.
  DataflowWorklist worklist(cfg);
  llvm::BitVector previouslyVisited(cfg.getNumBlockIDs());
  worklist.enqueueSuccessors(&cfg.getEntry());
  llvm::BitVector wasAnalyzed(cfg.getNumBlockIDs(), false);
  wasAnalyzed[cfg.getEntry().getBlockID()] = true;

  while (const CFGBlock *block = worklist.dequeue()) {
    // Did the block change?
    bool changed = runOnBlock(block, cfg, ac, vals, wasAnalyzed);
    ++stats.NumBlockVisits;
    if (changed || !previouslyVisited[block->getBlockID()])
      worklist.enqueueSuccessors(block);    
    previouslyVisited[block->getBlockID()] = true;
  }
  
  // Run through the blocks one more time, and report uninitialized variabes.
  for (CFG::const_iterator BI = cfg.begin(), BE = cfg.end(); BI != BE; ++BI) {
    const CFGBlock *block = *BI;
    if (wasAnalyzed[block->getBlockID()]) {
      runOnBlock(block, cfg, ac, vals, wasAnalyzed, &handler);
      ++stats.NumBlockVisits;
    }
  }
}

UninitVariablesHandler::~UninitVariablesHandler() {}
