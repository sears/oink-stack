// see License.txt for copyright and terms of use

#include "xform.h"              // this module
#include "xform_cmd.h"          // XformCmd
#include "xform_global.h"
#include "oink.gr.gen.h"        // CCParse_Oink
#include "strutil.h"            // quoted
#include "oink_util.h"
#include "mangle.h"             // mangle (type name mangler)

#include "Pork/patcher.h"       // Patcher

extern "C" {
#  include "proquint.h"         // proquint converters
#include <stdio.h>
}
#include <set>
#include <string.h>             // memset
#include <ctype.h>              // isalnum

// FIX: this analysis is incomplete

// FIX: the analysis should honor Variable_O::filteredOut() semantics.

// FIX: commonality with qual.cc stackness analysis: The heapify
// transformation done here has a lot in common with the stackness
// analysis in qual.cc; we should be using that to confirm our
// transformations here.
//
// Actually, the qual.cc stackness analysis doesn't seem to do the
// right thing: it looks for pointers from the heap to the stack, but
// we also need to know about pointers from the stack to the stack.

// **** utilities

static std::string getRange(Patcher &patcher, SourceLoc begin, SourceLoc end) {
  CPPSourceLoc ploc(begin);
  CPPSourceLoc ploc_end(end);
  PairLoc pairLoc(ploc, ploc_end);
  UnboxedPairLoc unboxedPairLoc(pairLoc);
  return patcher.getRange(unboxedPairLoc);
}

static void printPatch
  (Patcher &patcher, char const *str, SourceLoc begin, SourceLoc end)
{
  CPPSourceLoc ploc(begin);
  CPPSourceLoc ploc_end(end);
  PairLoc pairLoc(ploc, ploc_end);
  UnboxedPairLoc unboxedPairLoc(pairLoc);
  patcher.printPatch(str, unboxedPairLoc);
}

static bool dynSize(Type *type, SourceLoc loc) {
  bool ret = false;
  try {
    type->reprSize();
  } catch (XReprSize &e) {
    HANDLER();
    if (e.isDynamic) {
      ret = true;
    } else {
      // FIX: what to do here?
      userFatalError(loc, "Type that has neither static nor dynamic size ??");
    }
  }
  return ret;
}

// extract an alpha-num subsequence from a string that might remind a
// human of the original (thing of the unix utility "strings"); NOTE:
// looses information
static void get_strings(StringRef const str, stringBuilder &out) {
  int lastAlnum = true;         // don't start with underscores
  for (char const *cp = str; *cp; ++cp) {
    bool const isAlnum = isalnum(*cp);
    if (isAlnum) {
      // lazily append underscores so we don't end with underscores
      if (!lastAlnum) out << "_";
      out << (char) *cp;
    }
    // use a bool so as to deliberately loose information as to the
    // number of non-alnum-s in a row their were
    lastAlnum = isAlnum;
  }
}

// some supposedly prime numbers just over one billion; from
// http://publicliterature.org/tools/prime_number_generator/

// multiplier for hashing
#ifndef STR_HASHMULT
#  define STR_HASHMULT 1050506927U
#endif

// adder for hashing
#ifndef STR_HASHADD
#  define STR_HASHADD 1070708851U
#endif

static unsigned int hash_str(StringRef x) {
  // FIX: hash whole words at a time
  unsigned int hashvalue = STR_HASHADD;
  while(unsigned char const c = *x) {
    hashvalue += c;
    hashvalue *= STR_HASHMULT;
    // FIX: do a low-high byte swap here; FIX: why am I doing the add
    // last?
    hashvalue += STR_HASHADD;
    ++x;
  }
  return hashvalue;
}

// static void appendHexStr(unsigned int n, stringBuilder &ret) {
//   char *hex = NULL;
//   int printRes = asprintf(&hex, "%08x", n);
//   if (printRes < 0) {
//     // out of memory
//     fputs("out of memory\n", stderr);
//     exit(INTERNALERROR_ExitCode);
//   }
//   ret << hex;
//   free(hex);
// }

static void appendProquintStr(unsigned int n, stringBuilder &ret) {
  // Length of a 32-bit quint word, without trailing NUL: two quints
  // plus a separator.
  int const QUINT_LEN = 5*2 + 1;
  // Double length plus another separator in case we switch to
  // 64-bits.
  int const DOUBLE_QUINT_LEN = 2*QUINT_LEN + 1;
  char quint[DOUBLE_QUINT_LEN+1];
  // FIX: there has to be a faster way
//   for(int i=0; i<DOUBLE_QUINT_LEN+1; ++i) quint[i] = '\0';
  memset( (void*) quint,
          0x0,
          DOUBLE_QUINT_LEN+1 );
  uint2quint(quint, n, '_');
  ret << quint;
}

// Decides if this variable allocated on the stack.  Note that if it
// is in a class/struct/union we say no as it's container decides it.
//
// FIX: This function has a lot of duplication with qual.cc function
// void MarkVarsStackness_VisitRealVars::visitVariableIdem(Variable*);
static bool allocatedOnStack(Variable *var0) {
  Variable_O *var = asVariable_O(var0);
  xassert(var->getReal());
  switch(var->getScopeKind()) {
  case NUM_SCOPEKINDS: xfailure("can't happen"); break; // silly gcc warning
  case SK_UNKNOWN:              // not yet registered in a scope
    // FIX: the global scope has ScopeKind SK_UNKNOWN
//     xfailure("got SK_UNKNOWN on a real variable");
    return false;
    break;
  case SK_GLOBAL:               // toplevel names
//     markNonStack(var);
    return false;
    break;
  case SK_PARAMETER:            // parameter list
//     markStack(var);
    return true;
    break;
  case SK_FUNCTION:             // includes local variables
    if (var->hasFlag(DF_STATIC)) {
//       markNonStack(var); // basically it is global
      return false;
    } else {
//       markStack(var);
      return true;
    }
    break;
  case SK_CLASS:                // class member scope
    if (var->hasFlag(DF_STATIC)) {
//       markNonStack(var);        // basically it is global
      return false;
    } else {
      // do not mark as the storage-class depends on the containing
      // class; the containing variable will be reallocated onto the
      // heap if it is on the stack and not if it is not
      return false;           // we don't know that it is on the stack
    }
    break;
  case SK_TEMPLATE_PARAMS: // template paramter list (inside the '<' and '>')
  case SK_TEMPLATE_ARGS: // bound template arguments, during instantiation
    // not code
    xfailure("got template variable as a real variable");
    break;
  case SK_NAMESPACE:            // namespace
    // don't mark as can't contain data
    return false;
    break;
  }
  xfailure("can't happen");
}

// find the D_name nested down within an IDeclarator
static D_name *find_D_name(IDeclarator *decl) {
  if (false) {                  // orthogonality

  // "x" (NULL means abstract declarator or anonymous parameter);
  // this is used for ctors and dtors as well as ordinary names
  // (dtor names start with "~"); it's also used for operator names
//   -> D_name(PQName /*nullable*/ name);
  } else if (decl->isD_name()) {
    return decl->asD_name();

  // "*x" (as in "int *x")
//   -> D_pointer(CVFlags cv,  // optional qualifiers applied to ptr type
//                IDeclarator base);
  } else if (decl->isD_pointer()) {
    return find_D_name(decl->asD_pointer()->base);

  // "&x"
//   -> D_reference(IDeclarator base);
  } else if (decl->isD_reference()) {
    return find_D_name(decl->asD_reference()->base);

  // "f(int)"
//   -> D_func(IDeclarator base,                       // D_name of function, typically
//             FakeList<ASTTypeId> *params,            // params with optional default values
//             CVFlags cv,                             // optional "const" for member functions
//             ExceptionSpec /*nullable*/ exnSpec);    // throwable exceptions
  } else if (decl->isD_func()) {
    return find_D_name(decl->asD_func()->base);

  // "a[5]" or "b[]"
//   -> D_array(IDeclarator base, Expression /*nullable*/ size);
  } else if (decl->isD_array()) {
    return find_D_name(decl->asD_array()->base);

  // "c : 2"
  //
  // I use a PQName here instead of a StringRef for uniformity
  // (so every IDeclarator ends with a PQName); there are never
  // qualifiers on a bitfield name
//   -> D_bitfield(PQName /*nullable*/ name, Expression bits);
  } else if (decl->isD_bitfield()) {
    return NULL;

  // "X::*p"
//   -> D_ptrToMember(PQName nestedName, CVFlags cv, IDeclarator base);
  } else if (decl->isD_ptrToMember()) {
    return find_D_name(decl->asD_ptrToMember()->base);

  // declarator grouping operator: it's semantically irrelevant
  // (i.e. equivalent to just 'base' alone), but plays a role in
  // disambiguation
//   -> D_grouping(IDeclarator base);
  } else if (decl->isD_grouping()) {
    return find_D_name(decl->asD_grouping()->base);

  } else xfailure("can't happen");
}

// **** IssuesWarnings

void IssuesWarnings::warn(SourceLoc loc, string msg) {
  printLoc(loc);
  std::cout << "warning: " << msg << std::endl;
  warningIssued = true;
}

// **** OnlyDecltorsOfRealVars_ASTVisitor

// visit only declarators of real variables
class OnlyDecltorsOfRealVars_ASTVisitor : private ASTVisitor {
public:
  LoweredASTVisitor loweredVisitor; // use this as the argument for traverse()

  public:
  OnlyDecltorsOfRealVars_ASTVisitor()
    : loweredVisitor(this)
  {}
  virtual ~OnlyDecltorsOfRealVars_ASTVisitor() {}

  virtual bool visitPQName(PQName *);
};

bool OnlyDecltorsOfRealVars_ASTVisitor::visitPQName(PQName *obj) {
  // from RealVarAndTypeASTVisitor::visitPQName(PQName*): Scott points
  // out that we have to filter out the visitation of PQ_template-s:
  //
  // SGM 2007-08-25: Do not look inside PQ_template argument lists.
  // For template template parameters, the argument list may refer to
  // an uninstantiated template, but client analyses will just treat
  // the whole PQ_template as just a name; no need to look inside it.
  if (obj->isPQ_template()) {
    return false;
  }
  return true;
}

// **** AddrTaken_ASTVisitor

// make a set of variables that have had their addr taken; FIX: this
// analysis should probably be renamed to "address escapes" rather
// than just "addr taken" as just about any use of an array takes its
// address, but an immediate de-reference doesn't cause the address to
// escape unless there is an explicit addr-of (&) operator wrapped
// around it or more trickery from C++
class AddrTaken_ASTVisitor : public OnlyDecltorsOfRealVars_ASTVisitor {
public:
  SObjSet<Variable*> &addrTaken;

public:
  AddrTaken_ASTVisitor(SObjSet<Variable*> &addrTaken0)
    : addrTaken(addrTaken0)
  {}
  // visitor methods
  virtual bool visitExpression(Expression *);
#ifdef GNU_EXTENSION
  bool visitASTTypeof(ASTTypeof *);
#endif // GNU_EXTENSION

  // utility methods
private:
  // if this expression ultimately resolves to a variable, find it;
  // otherwise return NULL
  void registerUltimateVariable(Expression *);
};

#ifdef GNU_EXTENSION
bool AddrTaken_ASTVisitor::visitASTTypeof(ASTTypeof *obj) {
  if (obj->isTS_typeof_expr()) {
    return false;
  }
  return true;
}
#endif // GNU_EXTENSION

bool AddrTaken_ASTVisitor::visitExpression(Expression *obj) {
  if (obj->isE_addrOf()) {
    registerUltimateVariable(obj->asE_addrOf()->expr);
  } else if (obj->isE_sizeof()) {
    return false;
#ifdef GNU_EXTENSION
  } else if (obj->isE_alignofExpr()) {
    return false;
#endif // GNU_EXTENSION
  } else if (obj->isE_deref()) {
    E_deref *ederef = obj->asE_deref();
    if (ederef->ptr->isE_binary()) {
      E_binary *ebin = ederef->ptr->asE_binary();
      if (ebin->op == BIN_PLUS) {
        // NOTE: BIN_BRACKETS becomes this
        bool const leftIsArray = ebin->e1->type->asRval()->isArrayType();
        bool const rightIsInt =  ebin->e2->type->asRval()->isIntegerType();
        if (leftIsArray) {
          xassert(rightIsInt);    // how can this not be?
          // we want to prune the left child but not the right, so we
          // have to prune the whole thing and then launch the traversal
          // on the right
          ebin->e2->traverse(this->loweredVisitor);
          return false;           // prune
        }
      }
    }
  } else if (obj->isE_variable()) {
    // almost any use of an E_variable of array type takes the address
    // of the array: ISO/IEC 9899:1999 C99 spec 6.3.2.1 paragraph 3:
    //
    // "Except when it is the operand of the sizeof operator or the
    // unary & operator, or is a string literal used to initialize an
    // array, an expression that has type ''array of type'' is
    // converted to an expression with type ''pointer to type'' that
    // points to the initial element of the array object and is not an
    // lvalue. If the array object has register storage class, the
    // behavior is undefined."
    //
    // a string literal is not a variable, we already take the address
    // of arrays that are arguments to the addr-of operator, and we
    // filter out sizeof, typeof, and alignof in this traversal
    //
    // what we really care about is an escape analysis "is the address
    // taken and then stored somewhere?"; therefore we also filter out
    // immediate array uses, unless those got wrapped in an addr-of
    if (obj->asE_variable()->var->type->asRval()->isArrayType()) {
      registerUltimateVariable(obj);
    }
  }
  return true;
}

void AddrTaken_ASTVisitor::registerUltimateVariable(Expression *expr) {
  // Note that we do not do the right thing in the presence of
  // -fo-instance-sensitive, which is why xform_cmd.cc refuses to run
  // if you pass that flag.
  expr = expr->skipGroups();
  if (expr->isE_variable()) {
    addrTaken.add(expr->asE_variable()->var);
  } else if (expr->isE_fieldAcc()) {
    E_fieldAcc *efield = expr->asE_fieldAcc();
    Variable *field = efield->field;
    addrTaken.add(field);
    // the address of a field was taken so we consider the address of
    // the whole expression to have been taken:
    //   A a;
    //   &(a.x);
    // that technically takes the address of x but we consider it to
    // also take the address of a
    xassert(field->getScopeKind() == SK_CLASS);
    // static vars aren't actually allocated in their class
    if (!field->hasFlag(DF_STATIC)) {
      registerUltimateVariable(efield->obj);
    }
  } else if (expr->isE_deref()) {
    Expression *expr2 = expr->asE_deref()->ptr;
    if (!expr2->isE_binary()) {
      // if an E_addrOf of an E_deref results in a variable having its
      // address taken, then the variable's address was already taken
      // in the first place somewhere else and stored in a variable,
      // unless it is an immediate array plus an int (a lowered array
      // dereference)
    }
    E_binary *ebin = expr2->asE_binary();
    if (ebin->op == BIN_PLUS) {
      // NOTE: BIN_BRACKETS becomes this
      bool const leftIsArray = ebin->e1->type->asRval()->isArrayType();
      bool const rightIsInt = ebin->e2->type->asRval()->isIntegerType();
      if (leftIsArray) {
        xassert(rightIsInt);    // how can this not be?
        registerUltimateVariable(ebin->e1);
      }
    } else {
      xfailure("how can you deref this kind of binary expr?");
    }
  } else if (expr->isE_cond()) {
    // gcc complains, but it still lets you do it:
    //   &(argc==1 ? a : b)
    // addr_of_econd.c:6: warning: argument to '&' not really an
    // lvalue; this will be a hard error in the future
    E_cond *econd = expr->asE_cond();
    // because of this non-determinisim, we can't make
    // registerUltimateVariable return a variable and de-couple it from
    // addrTake.add()
    registerUltimateVariable(econd->th);
    registerUltimateVariable(econd->el);
  } else if (expr->isE_binary()) {
    E_binary *ebin = expr->asE_binary();
    if (ebin->op == BIN_COMMA) {
      registerUltimateVariable(ebin->e2);
    } else if (ebin->op == BIN_ASSIGN) {
      xfailure("taking the address of a BIN_ASSIGN not implemented");
    } else if (ebin->op == BIN_DOT_STAR) {
      xfailure("taking the address of a BIN_DOT_STAR not implemented");
    } else if (ebin->op == BIN_ARROW_STAR) {
      xfailure("taking the address of a BIN_ARROW_STAR not implemented");
    } else if (ebin->op == BIN_BRACKETS) {
      xfailure("E_binary BIN_BRACKETS should have been eliminated "
               "during typechecking");
    } else {
      xfailure("how can you take the address of this kind of binary expr?");
    }
  } else if (expr->isE_arrow()) {
    xfailure("E_arrow should have been eliminated during typechecking");
  } else {
    xfailure("how can you take the address of this kind of expression?");
  }
}

// **** VarDeclAndUse_ASTVisitor

class VarDeclAndUse_ASTVisitor
  : public OnlyDecltorsOfRealVars_ASTVisitor {
public:
  VarDeclAndUse_ASTVisitor() {}

  virtual bool visitDeclarator(Declarator *);
  virtual bool visitExpression(Expression *);

  bool subVisitE_variable(E_variable *);

  // are we interested in this variable?
  virtual bool pass(Variable *) = 0;
};

bool VarDeclAndUse_ASTVisitor::visitDeclarator(Declarator *obj) {
  Variable_O *var = asVariable_O(obj->var);
  if (pass(var)) {
    if (var->getScopeKind() == SK_PARAMETER) {
      printLoc(obj->decl->loc);
      std::cout << "param decl " << var->name << std::endl;
    } else if (var->getScopeKind() == SK_FUNCTION) {
      printLoc(obj->decl->loc);
      std::cout << "auto decl " << var->name << std::endl;
    } else xfailure("non-param non-auto var can't be stack allocated");
  }
  return true;
}

bool VarDeclAndUse_ASTVisitor::visitExpression(Expression *obj) {
  if (obj->isE_variable()) {
    return subVisitE_variable(obj->asE_variable());
  }
  return true;
}

bool VarDeclAndUse_ASTVisitor::subVisitE_variable(E_variable *evar) {
  // Note: if you compile without locations for expressions this will
  // stop working.
  Variable_O *var = asVariable_O(evar->var);
  if (pass(var)) {
    if (var->getScopeKind() == SK_PARAMETER) {
      printLoc(evar->loc);
      std::cout << "param use " << var->name << std::endl;
    } else if (var->getScopeKind() == SK_FUNCTION) {
      printLoc(evar->loc);
      std::cout << "auto use " << var->name << std::endl;
    } else xfailure("non-param non-auto var can't be stack allocated");
  }
  return true;
}

// **** PrintStackAllocVars_ASTVisitor

class PrintStackAllocVars_ASTVisitor
  : public VarDeclAndUse_ASTVisitor {
public:
  PrintStackAllocVars_ASTVisitor() {}

  // are we interested in this variable?
  virtual bool pass(Variable *);
};

bool PrintStackAllocVars_ASTVisitor::pass(Variable *var) {
  return allocatedOnStack(var);
}

// **** PrintStackAllocAddrTakenVars_ASTVisitor

class PrintStackAllocAddrTakenVars_ASTVisitor
  : public VarDeclAndUse_ASTVisitor {
public:
  SObjSet<Variable*> &addrTaken;

  PrintStackAllocAddrTakenVars_ASTVisitor(SObjSet<Variable*> &addrTaken0)
    : addrTaken(addrTaken0)
  {}

  // are we interested in this variable?
  virtual bool pass(Variable *);
};

bool PrintStackAllocAddrTakenVars_ASTVisitor::pass(Variable *var) {
  if (!allocatedOnStack(var)) return false;
  return addrTaken.contains(var);
}

// **** HeapifyStackAllocAddrTakenVars_ASTVisitor

// FIX: we should handle alloca()

// the scope created by an S_compound
class S_compound_Scope {
public:
  // the variables encountered in S_decls in this scope
  SObjStack<Variable> s_decl_vars;

  explicit S_compound_Scope() {}
};

class HeapifyStackAllocAddrTakenVars_ASTVisitor
  : public OnlyDecltorsOfRealVars_ASTVisitor {
public:
  IssuesWarnings &warn;
  SObjSet<Variable*> &addrTaken;
  Patcher &patcher;
  Function *root;               // root of the traversal
  SObjSet<Variable*> xformedVars;
  SObjStack<S_compound_Scope> scopeStack;

  HeapifyStackAllocAddrTakenVars_ASTVisitor
  (IssuesWarnings &warn0, SObjSet<Variable*> &addrTaken0,
   Patcher &patcher0, Function *root0)
    : warn(warn0)
    , addrTaken(addrTaken0)
    , patcher(patcher0)
    , root(root0)
  {}

  bool pass(Variable *);

  virtual bool visitFunction(Function *);
  virtual bool visitStatement(Statement *);
  virtual bool visitExpression(Expression *);

  virtual void postvisitStatement(Statement *);

  bool subVisitS_return(S_return *);
  bool subVisitS_decl(S_decl *);
  bool subVisitE_variable(E_variable *);

  char *xformDeclarator(Declarator *);
};

bool HeapifyStackAllocAddrTakenVars_ASTVisitor::pass(Variable *var) {
  if (!allocatedOnStack(var)) return false;
  return addrTaken.contains(var);
}

bool HeapifyStackAllocAddrTakenVars_ASTVisitor::
visitFunction(Function *obj) {
  // get the module for this function
  SourceLoc loc = obj->nameAndParams->decl->loc;
  StringRef module = moduleForLoc(loc);

  // do not transform default modules
  if (module == defaultModule) {
    printf("skipping function in default module: %s\n",
           obj->nameAndParams->var->name);
    return false;
  }

  if (obj == root) {
    // if this is the start of the traversal, we have visited this
    // S_compound before: skip this node and keep going down the tree
    return true;
  }

  // warn about any params
  SFOREACH_OBJLIST_NC(Variable, obj->funcType->params, iter) {
    Variable *paramVar = iter.data();
    xassert(paramVar->getScopeKind() == SK_PARAMETER);
    if (pass(paramVar)) {
      // we can't transform these so we just tell the user about them
      warn.warn(paramVar->loc,
                stringc << "param decl needs heapifying " << paramVar->name);
    }
  }

  // re-launch the traversal
  HeapifyStackAllocAddrTakenVars_ASTVisitor newEnv
    (this->warn, this->addrTaken,
     this->patcher, obj /*skip this node the second time*/);
  obj->traverse(newEnv.loweredVisitor);
  return false;         // IMPORTANT: prune the rest of the visitation
}

bool HeapifyStackAllocAddrTakenVars_ASTVisitor::
visitStatement(Statement *obj) {
  if (obj->isS_compound()) {
    scopeStack.push(new S_compound_Scope());
    return true;
  } else if (obj->isS_return()) {
    return subVisitS_return(obj->asS_return());
  } else if (obj->isS_decl()) {
    return subVisitS_decl(obj->asS_decl());
  } else if (obj->isS_goto()) {
    // we don't know if we have to free here or not; FIX: we might be
    // able to refine this
    warn.warn(obj->loc,
              stringc << "goto may require some variables to be free()-ed");
  } else if (obj->isS_break()) {
    // we don't know if we have to free here or not; FIX: we might be
    // able to refine this
    warn.warn(obj->loc,
              stringc << "break may require some variables to be free()-ed");
  } else if (obj->isS_continue()) {
    // we don't know if we have to free here or not; FIX: we might be
    // able to refine this
    warn.warn(obj->loc,
              stringc << "continue may require some variables to be free()-ed");
  }
  return true;
}

void HeapifyStackAllocAddrTakenVars_ASTVisitor::
postvisitStatement(Statement *obj) {
  if (obj->isS_compound()) {
    S_compound_Scope *scopeStackTop = scopeStack.pop();
    ASTList<Statement> *stmts = &obj->asS_compound()->stmts;
    if (!stmts->isEmpty()) {
      Statement *lastStatement = stmts->last();
      if (!lastStatement->isS_return()) {
        // the last statement of the block was not an S_return so we can
        // fall off the end of the block
        stringBuilder freeBlock;
        freeBlock << "{";
        int numVarsToFree = 0;
        SFOREACH_OBJLIST(Variable, scopeStackTop->s_decl_vars.list, iter) {
          ++numVarsToFree;
          freeBlock << xformCmd->free_func
                    << "(" << iter.data()->name << ");";
        }
        if (numVarsToFree > 0) {
          freeBlock << "};";
          CPPSourceLoc lastStatement_ploc_end(lastStatement->endloc);
          patcher.insertBefore(lastStatement_ploc_end, freeBlock.c_str());
        }
      }
    }
    delete scopeStackTop;
  }
}

// emit the free() expressions for all vars on the var stack of for
// all scopes on the scope stack
bool HeapifyStackAllocAddrTakenVars_ASTVisitor::
subVisitS_return(S_return *obj) {
  // get the current return string
  CPPSourceLoc ret_ploc(obj->loc);
  CPPSourceLoc ret_ploc_end(obj->endloc);
  PairLoc ret_PairLoc(ret_ploc, ret_ploc_end);
  UnboxedPairLoc ret_UnboxedPairLoc(ret_PairLoc);
  std::string retStr = patcher.getRange(ret_UnboxedPairLoc);

  // build the replacement
  stringBuilder newRet;
  newRet << "{";
  SFOREACH_OBJLIST(S_compound_Scope, scopeStack.list, iter) {
    SFOREACH_OBJLIST(Variable, iter.data()->s_decl_vars.list, iter2) {
      newRet << xformCmd->free_func << "(" << iter2.data()->name << ");";
    }
  }
  newRet << retStr.c_str() << "}";

  // replace it
  patcher.printPatch(newRet.c_str(), ret_UnboxedPairLoc);
  return true;
}

bool HeapifyStackAllocAddrTakenVars_ASTVisitor::subVisitS_decl(S_decl *obj) {
  Declaration *declaration = obj->decl;

  // find a Declarator to transform
  Declarator *declarator0 = NULL;
  FAKELIST_FOREACH_NC(Declarator, declaration->decllist, declarator) {
    if (pass(declarator->var)) {

      // skip those where we can't handle the initializer
      if (declarator->init) {
        Initializer *init = declarator->init;
        if (init->isIN_compound()) {
          warn.warn(declarator->decl->loc, 
                    stringc << "auto decl has compound initializer: "
                    << declarator->var->name);
          return false;
        } else if (init->isIN_ctor()) {
          warn.warn(declarator->decl->loc,
                    stringc << "auto decl has ctor initializer: "
                    << declarator->var->name);
          return false;
        }
        // punt on stack allocated arrays in general
        if (declarator->var->type->isArrayType()) {
          // FIX: Derrick pointed out initializing a heapified array
          // from a string literal:
          //
          //  * use strcpy() on the initializer
          //
          //  * copy the string literal to all the sizeof expressions
          //
          //  * including the new initializer
          //
          // Note that sizeof str literal gives the size you want for
          //  malloc: strlen() + 1.
          warn.warn(declarator->decl->loc,
                    stringc << "auto decl of an array: "
                    << declarator->var->name);
          return false;
        }
      }

      // we should transform this one
      if (declarator0) {
        // we can't handle this case yet
        warn.warn(declaration->decllist->first()->decl->loc,
                  stringc << "declaration having multiple declarators"
                  " which need heapifying");
        return true;
      } else {
        declarator0 = declarator;
      }
    }
  }

  // if nothing to do, leave
  if (!declarator0) return true;

  // accumulate the new initializer statements to put at the end of
  // this Declaration
  stringBuilder newInitStmts;

  // xform this Declarator
  {
    Variable *var = declarator0->var;
    char *oldInit_c_str = xformDeclarator(declarator0);
    if (oldInit_c_str) {
      newInitStmts << " *" << var->name << "=" << oldInit_c_str << ";";
      free(oldInit_c_str);
    }
    // record that we processed this var so we know to xform its uses
    xformedVars.add(var);
    // push the var onto the var stack in the top scope on scope stack
    scopeStack.top()->s_decl_vars.push(var);
  }

  // Insert the new initializers at the end of the S_decl.
  //
  // NOTE: one must in general be careful when turing one statement
  // (here an S_Decl) into multiple statements as the single statement
  // could be the statement argument to an 'if', 'else', 'while',
  // 'for', 'do'.  In the particular case of this analysis, it can't
  // happen because you can't stack allocate a var and take it's
  // address in one statement and you can't take it's address later
  // because the scope would have closed.  FIX: Maybe you could stack
  // allocate it and then in the initializer somehow take its address
  // and store it on the heap?
  if (newInitStmts.length()) {
    CPPSourceLoc s_decl_ploc_end(obj->endloc);
    patcher.insertBefore(s_decl_ploc_end, newInitStmts.c_str());
  }

  return true;
}

// Note: caller is responsible for freeing the return value; note: we
// optimize the empty string by just returning NULL
char *HeapifyStackAllocAddrTakenVars_ASTVisitor::
xformDeclarator(Declarator *obj) {
  xassert(obj->context == DC_S_DECL);

  Variable_O *var = asVariable_O(obj->var);
  xassert(pass(var));
  xassert(var->getScopeKind() == SK_FUNCTION);
  xassert(var->name);

  // find the D_name
  D_name *dname = find_D_name(obj->decl);
  // it shouldn't be possible to take the address of a bitfield and
  // that's the only way this can fail
  xassert(dname);
  CPPSourceLoc dname_ploc(dname->loc);
  CPPSourceLoc dname_ploc_end(dname->endloc);
  PairLoc dname_PairLoc(dname_ploc, dname_ploc_end);
  UnboxedPairLoc dname_UnboxedPairLoc(dname_PairLoc);
  if (!dname_PairLoc.hasExactPosition()) {
    // if the location is not exact, we can't insert anything
    warn.warn(obj->decl->loc,
              stringc << "FAIL: auto decl does not have exact start position: "
              << var->name);
    return NULL;
  }

  // find the end of the Declarator
  CPPSourceLoc decltor_ploc_end(obj->endloc);
  // if the location is not exact, we can't insert anything
  if (!decltor_ploc_end.hasExactPosition()) {
    warn.warn(obj->endloc,
              stringc << "FAIL: auto decl does not have exact end position: "
              << var->name);
    return NULL;
  }

  // fix the initializer
  char *oldInit_c_str = NULL;
  stringBuilder newInit;
  // FIX: not sure how this simplistic approach to names is going to
  // hold up in C++; if you don't want to use var->name you can use
  // this:
  //   StringRef name = dname->name->getName();
  newInit << "(typeof(" << var->name << "))"
          << xformCmd->xmalloc_func
          << "(sizeof *" << var->name << ")";
  if (obj->init) {
    xassert(obj->init->isIN_expr());
    // copy the old initializer
    CPPSourceLoc init_ploc(obj->init->loc);
    if (!init_ploc.hasExactPosition()) {
      warn.warn(obj->endloc,
                stringc << "FAIL: auto decl init does not have"
                " exact start position: " << var->name);
      return NULL;
    }
    PairLoc init_PairLoc(init_ploc, decltor_ploc_end);
    UnboxedPairLoc init_UnboxedPairLoc(init_PairLoc);
    oldInit_c_str = strdup(patcher.getRange(init_UnboxedPairLoc).c_str());
    // replace the old initializer with the new one
    patcher.printPatch(newInit.c_str(), init_UnboxedPairLoc);
  } else {
    // add the new initializer
    stringBuilder newInit2;
    newInit2 << "=" << newInit;
    patcher.insertBefore(decltor_ploc_end, newInit2.c_str());
    // attempt to do it with printPatch; doesn't work
//     // yes, replace the empty range
//     PairLoc decltor_end_PairLoc(decltor_ploc_end, decltor_ploc_end);
//     UnboxedPairLoc decltor_end_UnboxedPairLoc(decltor_end_PairLoc);
//     patcher.printPatch(newInit2.c_str(), decltor_end_UnboxedPairLoc);
  }

  // fix the D_name; note: the below doesn't work due to the inability
  // of Patcher to deal with multiple insertions at the same location
//   patcher.insertBefore(dname_ploc, "(*");
//   patcher.insertBefore(dname_ploc_end, ")");
  stringBuilder newInnerIDecl;
  newInnerIDecl << "(*" << var->name << ")";
  patcher.printPatch(newInnerIDecl.c_str(), dname_UnboxedPairLoc);

  return oldInit_c_str;
}

bool HeapifyStackAllocAddrTakenVars_ASTVisitor::
visitExpression(Expression *obj) {
  if (obj->isE_variable()) {
    return subVisitE_variable(obj->asE_variable());
  } else if (obj->isE_funCall()) {
    E_funCall *efun = obj->asE_funCall();
    StringRef funcName = funCallName_ifSimpleE_variable(efun);
    if (funcName) {
      if (streq("longjmp", funcName) || streq("siglongjmp", funcName)) {
        // we don't know if we have to free here or not; FIX: we might
        // be able to refine this
        warn.warn(obj->loc, stringc << funcName
                  << " may require some variables to be free()-ed");
      }
    }
  }
  return true;
}

bool HeapifyStackAllocAddrTakenVars_ASTVisitor::
subVisitE_variable(E_variable *evar) {
  // Note: if you compile without locations for expressions this will
  // stop working.
  Variable_O *var = asVariable_O(evar->var);

  // FIX: get rid of this
  if (pass(var)) {
    if (var->getScopeKind() == SK_PARAMETER) {
      // note that this is not a warning
      printLoc(evar->loc);
      std::cout << "param use " << var->name << std::endl;
    }
  }

  if (xformedVars.contains(var)) {
    xassert(var->getScopeKind() == SK_FUNCTION);
    CPPSourceLoc evar_ploc(evar->loc);
    CPPSourceLoc evar_ploc_end(evar->endloc);
    PairLoc evar_PairLoc(evar_ploc, evar_ploc_end);
    UnboxedPairLoc evar_UnboxedPairLoc(evar_PairLoc);
    stringBuilder newEvar;
    newEvar << "(*" << var->name << ")";
    patcher.printPatch(newEvar.c_str(), evar_UnboxedPairLoc);
  }
  return true;
}

// **** VerifyCrossModuleParams_ASTVisitor

class VerifyCrossModuleParams_ASTVisitor : private ASTVisitor {
public:
  LoweredASTVisitor loweredVisitor; // use this as the argument for traverse()
  IssuesWarnings &warn;
  StringRefMap<char const> *classFQName2Module;
  Patcher &patcher;

  VerifyCrossModuleParams_ASTVisitor
  (IssuesWarnings &warn0, StringRefMap<char const> *classFQName2Module0,
   Patcher &patcher0)
    : loweredVisitor(this)
    , warn(warn0)
    , classFQName2Module(classFQName2Module0)
    , patcher(patcher0)
  {}
  virtual ~VerifyCrossModuleParams_ASTVisitor() {}

  virtual bool visitFunction(Function *);
};

bool VerifyCrossModuleParams_ASTVisitor::visitFunction(Function *obj) {
  // get the module for this function
  SourceLoc loc = obj->nameAndParams->decl->loc;
  StringRef module = moduleForLoc(loc);

  // do not transform default modules
  if (module == defaultModule) {
    printf("skipping function in default module: %s\n",
           obj->nameAndParams->var->name);
    return false;
  }

  // look for the parameters that need to be verified and generate
  // code to verify them
  stringBuilder statusChecks;
  SFOREACH_OBJLIST_NC(Variable, obj->funcType->params, iter) {
    Variable_O *paramVar = asVariable_O(iter.data());
    xassert(paramVar->getScopeKind() == SK_PARAMETER);

    // is the parameter a pointer to a class/struct/union and that
    // class defined in a module that is not the same as the module of
    // this function?
    Type *paramTypeRval = paramVar->type->asRval();
    xassert(!paramTypeRval->isArrayType());
    if (!paramTypeRval->isPointerType()) continue;
    Type *paramAtType = paramTypeRval->asPointerType()->atType;
    StringRef lookedUpModule = moduleForType(classFQName2Module, paramAtType);
    if (!lookedUpModule) {
      // anonymous type: we can't tell what module this belongs to, so
      // we don't know if we should verify it or not
      warn.warn(paramVar->loc,
                stringc << paramVar->name
                << "has a type for which we cannot determine the module");
      continue;
    }
    if (module != lookedUpModule) continue;

    // **** make code to verify the status of the parameter

    // make a new name for the parameter; note: if this collides with
    // another name, the subsequent compile will give us an error
    stringBuilder newParamName;
    newParamName << paramVar->name << xformCmd->verify_param_suffix;
    // rename the parameter in situ
    CPPSourceLoc paramLoc(paramVar->loc);
    patcher.insertBefore(paramLoc, xformCmd->verify_param_suffix,
                         newParamName.length());
    // add a call to verify to filter the parameter value
    statusChecks << "typeof(" << newParamName.c_str() << ") "
                 << paramVar->name << "="
                 << xformCmd->verify_func
                 << "(" << newParamName.c_str() << ");";
  }

  // insert the status checks at the top of the function body
  if (statusChecks.length()) {
    CPPSourceLoc body_ploc(obj->body->loc);
    patcher.insertBefore(body_ploc, statusChecks.c_str(), 1);
  }

  return true;
}

// **** LocalizeHeapAlloc_ASTVisitor

class LocalizeHeapAlloc_ASTVisitor : public AllocSites_ASTVisitor {
public:
  IssuesWarnings &warn;
  StringRefMap<char const> *classFQName2Module;
  Patcher &patcher;

  LocalizeHeapAlloc_ASTVisitor
  (IssuesWarnings &warn0, StringRefMap<char const> *classFQName2Module0,
   Patcher &patcher0)
    : warn(warn0)
    , classFQName2Module(classFQName2Module0)
    , patcher(patcher0)
  {}

  // from superclass:
  virtual bool visitExpression(Expression *);
  virtual bool isAllocator0(E_funCall *, SourceLoc);
  virtual void subVisitCast0(Expression *cast, Expression *expr);
  virtual void subVisitE_new0(E_new *);

  virtual bool visitFunction(Function *obj);

  void localize_heapExpr(SourceLoc loc, SourceLoc endloc,
                         StringRef funcName, StringRef argStr,
                         Type *heapObjType, bool heapObjType_dynSize);
};

void LocalizeHeapAlloc_ASTVisitor::subVisitE_new0(E_new *) {
  // there is nothing to do here; FIX: we just have to imlement
  // operator new() for this class
}

bool LocalizeHeapAlloc_ASTVisitor::visitFunction(Function *obj) {
  // get the module for this function
  SourceLoc loc = obj->nameAndParams->decl->loc;
  StringRef module = moduleForLoc(loc);

  // do not transform default modules
  if (module == defaultModule) {
    // note that this is not a warning
    printf("skipping function in default module: %s\n",
           obj->nameAndParams->var->name);
    return false;
  }

  return true;
}

bool LocalizeHeapAlloc_ASTVisitor::isAllocator0
  (E_funCall *efun, SourceLoc loc)
{
  StringRef funcName = funCallName_ifSimpleE_variable(efun);
  if (!funcName) return false;
  if (isStackAllocator(funcName)) {
    warn.warn(loc,
              stringc << "stack allocator cannot be localized");
    // if you have to cast the return value of alloca() then we could
    // return true here, but we don't care because we can't do
    // anything to alloca() anyway so we don't care if we happen to
    // miss a call to it
  } else if (isHeapReAllocator(funcName)) {
    warn.warn(loc,
              stringc << "localization of heap re-allocator not implemented");
    return true;
  } else if (isHeapNewAllocator(funcName)) {
    return true;
  }
  return false;
}

void LocalizeHeapAlloc_ASTVisitor::subVisitCast0
  (Expression *cast, Expression *expr)
{
  // is this a cast of a call to a heap allocator?
  if (!expr->isE_funCall()) return;
  E_funCall *efun = expr->asE_funCall();
  StringRef funcName = funCallName_ifSimpleE_variable(efun);
  if (!funcName) return;
  if (!isHeapNewAllocator(funcName)) return;

  // is this one that we can transform?
  if (!streq(funcName, "malloc") && !streq(funcName, "xmalloc")) {
    warn.warn(expr->loc,
              stringc << "localization of heap new allocator"
              " other than 'malloc' not implemented");
    return;
  }

  // **** it's a call to (x)malloc()

  // get the type that we are allocating
  Type *heapObjType = cast->type->asRval()->asPointerType()->atType->asRval();

  // check there is one argument and it is "sizeof(heapObjType)"
  FakeList<ArgExpression> *args = efun->args;
  USER_ASSERT(args->count()==1, efun->loc,
              "(x)malloc does not have exactly 1 argument.");
  Expression *arg = args->first()->expr;
  if (arg->isE_sizeof()) {
    USER_ASSERT(arg->asE_sizeof()->expr->type->asRval()->equals(heapObjType),
                arg->loc,
                "(x)malloc argument is not sizeof-expr the cast at-type.");
  } else if (arg->isE_sizeofType()) {
    USER_ASSERT(arg->asE_sizeofType()->atype->getType()->asRval()->
                equals(heapObjType),
                arg->loc,
                "(x)malloc argument is not sizeof-type of the cast at-type.");
  } else {
    userFatalError
      (arg->loc, "Malloc argument is not sizeof-expr nor sizeof-type.");
  }

  // does the allocated type have static or dynamic size?
  bool heapObjType_dynSize = dynSize(heapObjType, cast->loc);

  // if type has a dynamic size then keep the size argument
  StringRef argStr = "";
  if (heapObjType_dynSize) {
    // FIX: this is kind of an abuse of globalStrTable
    argStr = globalStrTable(getRange(patcher, arg->loc, arg->endloc).c_str());
  }

  // replace the entire cast expression with a call to the mangled
  // malloc; yes, we get rid of the cast as well as that helps us
  // ensure type safety at static time
  localize_heapExpr(cast->loc, cast->endloc,
                    funcName, argStr,
                    heapObjType, heapObjType_dynSize);
}

bool LocalizeHeapAlloc_ASTVisitor::visitExpression(Expression *obj) {
  bool ret = AllocSites_ASTVisitor::visitExpression(obj);
  if (obj->isE_funCall()) {
    E_funCall *efun = obj->asE_funCall();
    StringRef funcName = funCallName_ifSimpleE_variable(efun);
    if (!funcName) return false;
    if (isHeapDeAllocator(funcName)) {
      if (!streq(funcName, "free")) {
        warn.warn(obj->loc,
                  stringc << "localization of heap de-allocator"
                  " not implemented");
      }

      // **** it's a call to free()

      // check there is one argument
      FakeList<ArgExpression> *args = efun->args;
      USER_ASSERT(args->count()==1, efun->loc,
                  "free does not have exactly 1 argument.");
      Expression *arg = args->first()->expr;

      // check it is of pointer type
      Type *argType = arg->type->asRval();
      USER_ASSERT(argType->isPointerType(), efun->loc,
                  "free of expression not of pointer type.");

      // get the type of the object being freed
      Type *heapObjType = argType->asPointerType()->atType->asRval();

      // does the allocated type have static or dynamic size?
      bool heapObjType_dynSize = dynSize(heapObjType, efun->loc);

      // get the string representing the argument
      // FIX: this is kind of an abuse of globalStrTable
      StringRef argStr =
        globalStrTable(getRange(patcher, arg->loc, arg->endloc).c_str());

      // replace the entire free() call
      localize_heapExpr(obj->loc, obj->endloc,
                        funcName, argStr,
                        heapObjType, heapObjType_dynSize);

    } else if (isHeapSizeQuery(funcName)) {
      warn.warn(obj->loc,
                stringc << "localization of heap size query not implemented");
    }
  }
  return ret;
}

// localize expressions that do heap operationss
void LocalizeHeapAlloc_ASTVisitor::
localize_heapExpr(SourceLoc loc, SourceLoc endloc,
                  StringRef funcName, StringRef argStr,
                  Type *heapObjType, bool heapObjType_dynSize)
{
  // get the module this code is in
  StringRef allocatorModule = moduleForLoc(loc);
  StringRef heapObjTypeModule = moduleForType(classFQName2Module, heapObjType);
  if (heapObjTypeModule) {
    USER_ASSERT(streq(allocatorModule, heapObjTypeModule), loc,
                "Allocated type defined in different module than "
                "call to allocator.");
  } else {
    // FIX: I don't think this is sound.
    //
    // anonymous type: we can't tell if it is allocated in the right
    // module; NOTE: we are trusting that the right module is the
    // module of this file; that is, that another analysis would have
    // found already if it were being allocated in the wrong module
  }

  // get the Elsa-mangled name of the type
  StringRef mangledTypeName = globalStrTable(mangle(heapObjType));

  // unique alphanum identifier for this class
  stringBuilder alnumClassName;
  // alphanum substrs of the mangledTypeName to preserve readability
  get_strings(mangledTypeName, alnumClassName);
  // hash of the mangledTypeName to preserve information
  alnumClassName << "__";
  appendProquintStr(hash_str(mangledTypeName), alnumClassName);
//   appendHexStr(hash_str(mangledTypeName), alnumClassName);

  // new alloc: old_name + mangled_type_name + module_name + (args)
  stringBuilder newHeapExpr;
  newHeapExpr << funcName;
  newHeapExpr << "__" << alnumClassName;
  newHeapExpr << "__" << allocatorModule;
  newHeapExpr << "(" << argStr << ")";

  // replace the expression with a call to the new heap expression
  printPatch(patcher, newHeapExpr.c_str(), loc, endloc);

  // record that we mangled this type; for each file emit a
  // corresponding configuration file listing for each class:
  std::cout
    << "localize: "
    // the modue name
    << "module:" << allocatorModule << ", "
    // the alnum class name,
    << "alnum-class:" << alnumClassName << ", "
    // whether the class has a dynamic size or not,
    << (heapObjType_dynSize ? "size:dyn" : "size:sta") << ", "
    // the C name of the class; despite the name, this is a good
    // pretty-printed string representation of the type,
    << "c-type:" << mangledTypeName
    << std::endl;
}

// **** IntroFunCall_ASTVisitor

// transform calls by inserting introduction code at the call site
class IntroFunCall_ASTVisitor : private ASTVisitor {
public:
  LoweredASTVisitor loweredVisitor; // use this as the argument for traverse()
  Patcher &patcher;

  public:
  IntroFunCall_ASTVisitor(Patcher &patcher0)
    : loweredVisitor(this)
    , patcher(patcher0)
  {}
  virtual ~IntroFunCall_ASTVisitor() {}

  virtual void postvisitExpression(Expression *);
};

void IntroFunCall_ASTVisitor::postvisitExpression(Expression *obj) {
  if (!obj->isE_funCall()) return;

  // get the current function call string
  CPPSourceLoc call_ploc(obj->loc);
  CPPSourceLoc call_ploc_end(obj->endloc);

  // FIX: it is inefficient to do this every time, but orthogonal
  stringBuilder newCall_start, newCall_end;

  // Get the byte-for-byte contents of the function call site.
  PairLoc call_PairLoc(call_ploc, call_ploc_end);
  UnboxedPairLoc call_UnboxedPairLoc(call_PairLoc);
  std::string call_str = patcher.getRange(call_UnboxedPairLoc);

  newCall_start << "({" << xformCmd->intro_fun_call_str << "; typeof(" << call_str.c_str() /*obj->exprToString()*/ << ") ret = ";
  newCall_end   << "; " << xformCmd->intro_fun_ret_str << "; ret; })";

  // note: multiple insertBefore() calls at the same location only
  // preserve the last one; I think this is safe because two function
  // calls cannot abut
  patcher.insertBefore(call_ploc, newCall_start.c_str());
  patcher.insertBefore(call_ploc_end, newCall_end.c_str());

  // this doesn't work due to bugs and/or missing features in Patcher;
  // doesn't work when run top-down in visitExpression() either
//   PairLoc call_PairLoc(call_ploc, call_ploc_end);
//   UnboxedPairLoc call_UnboxedPairLoc(call_PairLoc);
//   std::string callStr = patcher.getRange(call_UnboxedPairLoc);
//
//   // build the replacement
//   stringBuilder newCall;
//   newCall << "({"
//           << xformCmd->intro_fun_call_str
//           << callStr.c_str()
//           << "})";
//   // replace it
//   patcher.printPatch(newCall.c_str(), call_UnboxedPairLoc);
}

class WrapFunCall_ASTVisitor : private ASTVisitor {
private:
  std::set<std::string> wrapped_funcs;
  typedef std::pair<std::string, std::string> val;
  typedef std::pair<std::string, val > pair;
  std::map<std::string, val> known_types;

public:
  LoweredASTVisitor loweredVisitor;
  Patcher &patcher;

  WrapFunCall_ASTVisitor(Patcher &patcher0, const char * configFile)
    : loweredVisitor(this)
    , patcher(patcher0)
  {

    FILE * config = fopen(configFile, "r");
    char * lineptr = NULL;
    size_t lenptr = 0;
    ssize_t sz;
    while(-1 != (sz = getline(&lineptr, &lenptr, config))) {
      // the -1 strips the newline
      std::string s(lineptr, sz -1);
      wrapped_funcs.insert(s);
    }
    known_types.insert(pair("timeval", val("%lld, %lld", "(long long)%.tv_sec, (long long)%.tv_usec")));
    known_types.insert(pair("timespec", val("%lld, %lld", "(long long)%.tv_sec, (long long)%.tv_nsec")));

    known_types.insert(pair("recordid", val("%lld, %d, %d", "%.page, %.slot, %.size")));
    known_types.insert(pair("page_impl", val("%d, %d", "%.page_type, %.has_header")));
    known_types.insert(pair("stasis_operation_impl", val("%d, %d, %d, %d", "%.id, %.page_type, %.redo, %.undo")));
    known_types.insert(pair("stasis_transaction_table_entry_t", val("%d, %d, %lld, %lld, %ld, %ld, %ld, %d",
                            "%.xid, %.xidWhenFree, %.prevLSN, %.recLSN, %.commitArgs[0], %.commitArgs[1], %.commitArgs[2], %.tid")));
    fclose(config);

  }
#define WRAP_PREFIX "___wrapper_"
  virtual ~WrapFunCall_ASTVisitor() {}

  virtual void postvisitExpression(Expression *);
  virtual void postvisitTopForm(TopForm *);
  virtual void postvisitFunctionTF(TopForm *);
  virtual void postvisitDeclarationTF(TopForm *);
private:
//#define STRESS_TEST

  const IDeclarator *  derefDeclarator(const IDeclarator * deref_decl);
  const IDeclarator *  derefDeclaratorGrouping(const IDeclarator * deref_decl);
  bool resolve_stripped_type(
      const IDeclarator * decl,   // Contains line number for error messages.
      const TypeSpecifier * spec, // Use the AST to get info about names types (structs, typedefs, etc)..
      const Type * type,          // Use the output from the typechecker for everything else.
      // The following three are OUT params.  TODO: For now, we just leak the memory assocaited with them.
      const char ** ret_param_name,  // This is the name of the parameter.
      const char ** ret_fmt_string,  // This is the format string for printf.
      const char ** ret_param_call); // This string contains the argument(s) that should be passed to printf.
  bool resolve_stripped_type(
      const IDeclarator * decl,    // Contains line number for error messages
      FakeList<ASTTypeId>* ast_params, // Information from AST.
      SObjList<Variable>* type_params, // Information from type checker.
      int i,           // index into ast_params, type_params.  Must be a valid index into type_params (ie: callers must strip "void" from f(void) by observing that the length of type_params is zero.).
      const char ** param_name, const char ** fmt_string, const char ** param_call // OUT params, as above.
      );
};

// Given the AST of a function parameter, and a pointer to a type, compute a type id, format strings, etc..
const IDeclarator * WrapFunCall_ASTVisitor::derefDeclarator(const IDeclarator * deref_decl) {
  while(deref_decl->isD_pointer() || deref_decl->isD_grouping()) {
    while(deref_decl->isD_grouping()) {
      deref_decl = deref_decl->asD_groupingC()->base;
    }
    while(deref_decl->isD_pointer()) {
      deref_decl = deref_decl->asD_pointerC()->base;
    }
  }
  return deref_decl;
}
const IDeclarator * WrapFunCall_ASTVisitor::derefDeclaratorGrouping(const IDeclarator * deref_decl) {
  while(deref_decl->isD_grouping()) {
    deref_decl = deref_decl->asD_groupingC()->base;
  }
  return deref_decl;
}

bool WrapFunCall_ASTVisitor::resolve_stripped_type(
    const IDeclarator * decl,
    FakeList<ASTTypeId>* ast_params,
    SObjList<Variable> * type_params,
    int i,
    const char ** ret_param_name,
    const char ** ret_fmt_string,
    const char ** ret_param_call) {

  // try to find a name
  // strip pointers and groupings from name
  const IDeclarator * deref_decl = derefDeclarator(ast_params->nthC(i)->decl->decl);
  const char * param_name;
  bool is_void = false;
  bool ok = true;

  if(deref_decl->isD_name()) {
    const PQName * nm = deref_decl->asD_nameC()->name;
    if(nm == 0) {
      // OK, this is an unnamed parameter.  We don't really handle these in definitions, but the most common (only possible?)
      // case is f(void), which we special case.
      is_void = true;
      std::cerr << decl->loc << " Dead(?) code reached." << std::endl; // TODO remove dead code (callers handle this case...)
    } else {
      param_name = nm->getName();
    }
  } else if(deref_decl->isD_func()) {
    param_name = type_params->nthC(i)->name; // presumably, since this parameter type is not void, there will be a name.
  } else {
    std::cerr << decl->loc << " WTF is a " << deref_decl->kindName() << " doing in a param list?" << std::endl;
    ok = false;
  }
  if(ok) {
    *ret_param_name = is_void ? NULL : param_name;
    return resolve_stripped_type(decl, ast_params->nthC(i)->spec, type_params->nthC(i)->type, ret_param_name, ret_fmt_string, ret_param_call);
  } else {
    return false;
  }
}
bool WrapFunCall_ASTVisitor::resolve_stripped_type(
    const IDeclarator * decl,
    const TypeSpecifier * spec,
    const Type * type,
    const char ** ret_param_name,
    const char ** ret_fmt_string,
    const char ** ret_param_call) {
  static const std::string fmt_strings[] = {
      "%c",  //  ST_CHAR,
      "%c",  //  ST_UNSIGNED_CHAR,
      "%c",  //  ST_SIGNED_CHAR,
      "%d",  //  ST_BOOL,
      "%d",  //  ST_INT,
      "%u",  //  ST_UNSIGNED_INT,
      "%ld", //  ST_LONG_INT,
      "%lu", //  ST_UNSIGNED_LONG_INT,
      "%lld",//  ST_LONG_LONG,              // GNU/C99 extension
      "%llu",//  ST_UNSIGNED_LONG_LONG,     // GNU/C99 extension
      "%hd", //  ST_SHORT_INT,
      "%hu", //  ST_UNSIGNED_SHORT_INT,
      "%hu", //  ST_WCHAR_T,
      "%f",  //  ST_FLOAT,
      "%f",  //  ST_DOUBLE,
      "%lf", //  ST_LONG_DOUBLE,
      "%f",  //  ST_FLOAT_COMPLEX,          // GNU/C99 (see doc/complex.txt) // XXX how to printf complex types?
      "%f",  //  ST_DOUBLE_COMPLEX,         // GNU/C99
      "%lf", //  ST_LONG_DOUBLE_COMPLEX,    // GNU/C99
      "%f",  //  ST_FLOAT_IMAGINARY,        // C99
      "%f",  //  ST_DOUBLE_IMAGINARY,       // C99
      "%lf", //  ST_LONG_DOUBLE_IMAGINARY,  // C99
      "void" //  ST_VOID,                   // last concrete type (see 'isConcreteSimpleType')
  };
  bool ok = true;

  SimpleTypeId id  = ST_NOTFOUND;
  const char * typedefed_name  = 0;

  // We have a type already, but it could be that we need to get a name from some typedef junk, so check for that first.
  if(type->isPointerOrArrayRValueType()) {
    // skip the rest of this conditional
  } else if (spec->isTS_simple()) {
    // good
    id = spec->asTS_simpleC()->id;
    if(!isConcreteSimpleType(id)) {
      std::cerr << decl->loc << " WTF got a non-concrete simple type! " << std::endl;
      ok = false;
    }
  } else if(spec->isTS_name()) {
    // fall back on the type params, but remember the name
    typedefed_name = spec->asTS_nameC()->name->getName();
  } else if(spec->isTS_elaborated()) {
    // no-op; hopefully we'll do the right thing below.
  } else {
    std::cerr << decl->loc << " WTF is a " << spec->kindName() << " typespec?" << std::endl;
    ok = false;
  }
  if(ok && id == ST_NOTFOUND) { // we fell back on the type parameter
    if(type->isPointerOrArrayRValueType()) {
      id = ST_LONG_INT;
    } else if(type->isSimpleType()) {
      id = type->asSimpleTypeC()->type;
      if(!isConcreteSimpleType(id)) {
        std::cerr << decl->loc << " WTF got a non concrete simple type! " << type->toString() << " " << simpleTypeName(id) << std::endl;
        ok = false;
      }
    } else if(type->isEnumType()) {
      id = ST_INT; // XXX is this always (ever) true?
    } else if(type->isStructType()) {
      // handle this case below
    } else {
      std::cerr << decl->loc << " WTF got a type that I don't know about: " << type->toString() << std::endl;
      ok = false;
    }
    if(id == ST_VOID) {
      std::cerr << decl->loc << " WTF decided a type was void!" << type->toString() << std::endl;
    }
  }

  if(ok) {
    if(id == ST_NOTFOUND) {
      if(typedefed_name == 0) {
        std::string s = toCStr(type->toString());
        size_t i;
        if(std::string::npos != (i = s.find(' '))) {
          s = s.substr(0,i); // handle types like foo const.  TODO If ever there could be a const foo, then this will break.
        }
        typedefed_name = strdup(s.c_str());  // XXX memory leak
      }
      if(known_types.find(typedefed_name) != known_types.end()) {
        // look fmt_string and param_call up, return them.
        val v = (*known_types.find(typedefed_name)).second;
        *ret_fmt_string = v.first.c_str();
        std::string param = v.second.c_str();
        size_t i;
        while(std::string::npos != (i = param.find('%'))) {
          param.replace(i, 1, *ret_param_name ? *ret_param_name : "___ret");
        }
        *ret_param_call = strdup(param.c_str()); /// XXX memory leak
      } else {
        std::cerr << decl->loc << " WTF Encountered struct that I don't know how to print: " << typedefed_name << std::endl;
        ok = false;
      }
    } else {
      *ret_fmt_string = fmt_strings[id].c_str();
      if(*ret_param_name) {
        *ret_param_call = *ret_param_name; // XXX this can't be good for memory management, since we malloc return values above
      } else {
        *ret_param_call = "___ret"; // XXX is returning this even legal?
      }
    }
  }
  return ok;
}

void WrapFunCall_ASTVisitor::postvisitTopForm(TopForm * tf) {
  if(tf->isTF_func()) {
    postvisitFunctionTF(tf);
  } else if(tf->isTF_decl()) {
    postvisitDeclarationTF(tf);
  } else {
    return;
  }
}
void WrapFunCall_ASTVisitor::postvisitFunctionTF(TopForm * tf) {
  const Function * obj = tf->asTF_funcC()->f;
  bool ok = true;

  const IDeclarator * deref_decl = derefDeclarator(obj->nameAndParams->decl);

  if(!deref_decl->isD_func()) {
    std::cerr << deref_decl->loc << ": WTF is a " << deref_decl->kindName() << std::endl;
    ok = false;
  }

  const D_func * dfunc = 0;
  const char * name = 0;
  if(ok) {
    dfunc = deref_decl->asD_funcC();
    name = dfunc->base->asD_nameC()->name->getName();
  }

#ifndef STRESS_TEST
  if(wrapped_funcs.find(name) == wrapped_funcs.end()) {
    return; // not interested in this function.
  }
#endif

  int paramcount;

  if(ok) {
    if(obj->funcType->acceptsVarargs()) {
      std::cerr << deref_decl->loc << "WTF Function " << name << " wants varargs " << std::endl;
      ok = false;
    } else {
      paramcount = obj->funcType->params.count(); //dfunc->params->count() would return one for f(void), so use funcType instead.
    }
  }

  std::string retstr;

  if(ok) {
    // Build the part of declaration before the function name.

    // obj->retspec->loc is sporadically before or after static inline. use topform instead.
    PairLoc ret_range = PairLoc(tf->loc, dfunc->loc);
    if(!ret_range.hasExactPosition()) {
      std::cerr << "Warning: Cannot patch function " << name << "; could not figure out where it was defined." << std::endl;
      ok = false;
    } else {
      retstr = patcher.getRange(UnboxedPairLoc(ret_range));
    }
  }

  std::string paramstr;

  if(ok) {
    // Build the part of the declaration after the function name.
    if(paramcount > 0) {
      // Can't use obj->body->loc, since it includes any c prepreprocessor stuff between the ) and the {.
      // XXX this line of code hits a bug in oink.  The decl->endloc is sometimes one character too late.
      SourceLoc endofdecl = dfunc->params->nthC(dfunc->params->count() - 1)->decl->endloc;
      PairLoc range = PairLoc(dfunc->base->asD_name()->endloc, endofdecl);
      if(!range.hasExactPosition()) {
        std::cerr << " Error: Cannot patch function " << name << "; could not figure out where it was defined." << std::endl;
        ok = false;
      } else {
        paramstr = patcher.getRange(UnboxedPairLoc(range)) + ")";
      }
    } else {
      paramstr = "()";
    }
  }

  // Build printf invocation

  // This code is kind of nasty.  Ideally, we'd walk the type information,
  // and round-trip it from the type checker into a string that would
  // compile.  Unfortunately, the type checker erased the typedef
  // information at this point in the game, so we get all hacky, and resort
  // to string manipulation instead.
  const char * name_str = 0;
  const char * fmt_str = 0;
  const char * param_str = 0;
  bool returns_value;
  stringBuilder beforePrintf;
  stringBuilder afterPrintf;
  stringBuilder retArg;
  stringBuilder retFromWrapper;
  if(ok) {
    ok = resolve_stripped_type(deref_decl, obj->retspec, obj->funcType->retType, &name_str, &fmt_str, &param_str);
  }
  if(ok) {
    beforePrintf << "printf(\"call_" << name << "(%lld";
    if(strcmp (fmt_str, "void") == 0) {
      returns_value = false;
      afterPrintf << "printf(\"ret_" << name << "(%lld";
    } else {
      returns_value = true;
      afterPrintf  << "printf(\"ret_"  << name << "(%lld, " << fmt_str;
      retArg << ", " << param_str;
      retFromWrapper << "return ___ret;\n";
    }
  }

  stringBuilder invocationParams;
  if(ok) {  // Build printf strings and arguments.
    invocationParams << "(";
    typeof(obj->funcType->params) type_params = obj->funcType->params;

    stringBuilder printfArgs;
    printfArgs << ", *stasis_dbug_timestamp";
    for(int i = 0; ok && i < paramcount; i++) {
      ok = resolve_stripped_type(deref_decl, dfunc->params, &type_params, i, &name_str, &fmt_str, &param_str);
      if(ok) {
        if(i > 0) {
          beforePrintf << ", ";
          invocationParams << ", ";
        }
        if(i > 0 || returns_value) {
          afterPrintf  << ", ";
        }
        beforePrintf << fmt_str;
        afterPrintf  << fmt_str;
        printfArgs << ", " << param_str;
        invocationParams << name_str;
      }
    }
    beforePrintf << ")\\n\"" << printfArgs <<  ");";
    afterPrintf  << ")\\n\"" << retArg << printfArgs << ");";
    invocationParams << ")";
  }

  if(ok) {

    // Actually assemble the patch.

    stringBuilder invocation;
    invocation << name << invocationParams;

    stringBuilder p = "\n";

    p << retstr.c_str() << WRAP_PREFIX << name << paramstr.c_str() << "{\n"
      <<   "\t" << beforePrintf << "\n";

    if(returns_value) {
      p << "\ttypeof(" << invocation.c_str() << ") ___ret = ";
    } else {
      p << "\t";
    }
    p         << invocation.c_str() << ";\n"
      << "\t" << afterPrintf.c_str() << "\n"
      << "\t" << retFromWrapper
      << "}\n";

    patcher.insertBefore(obj->body->endloc, p.c_str()); // TODO Make sure this is always right.  (Can there be trailing crap after the body?)
  } else {
    std::cerr << deref_decl->loc << " Error: Cannot patch function " << name << std::endl;
  }
}
// XXX this misses function declarations inside of structs and function bodies.
// For now, this is preferable to losing the "static" and "inline" modifiers, but is still painful.
// We could use a normal postvisitDeclarationTF, except that we to copy strings like "static" and
// "inline" from in front of the declaration.
void WrapFunCall_ASTVisitor::postvisitDeclarationTF(TopForm *tf) {
  Declaration * obj = tf->asTF_declC()->decl;
  // iterate over the decllist.  TODO why is it a list? is this code doing the right thing if it is a list?
  int funcidx = -1;
  for(int i = 0; i < obj->decllist->count(); i++) {
    // We avoid function pointers for now.
    if(derefDeclaratorGrouping(obj->decllist->nthC(i)->decl)->isD_func()) {
      if(funcidx != -1) {
        std::cerr << obj->spec->loc << "WTF found multiple func decls" << std::endl;
      }
      funcidx = i;
    }
  }
  if(funcidx == -1) return;

  if(funcidx != 0) { std::cerr << obj->spec->loc << " postvisitDeclarationTF found func later on in list.  Check patch output." << std::endl; }
  const D_func * func = derefDeclaratorGrouping(obj->decllist->nth(funcidx)->decl)->asD_funcC();
  const IDeclarator * deref_base = derefDeclaratorGrouping(func->base);
  if(deref_base->isD_name()) {
    const D_name * name = deref_base->asD_nameC();
    const char* pqname = name->name->getName();
#ifndef STRESS_TEST
    if(wrapped_funcs.find(pqname) != wrapped_funcs.end()) {
#endif
      PairLoc firstPairLoc(tf->loc,
                           name->loc       // should be OK.
                           );
      PairLoc secondPairLoc(name->endloc,   // should be OK.
                            obj->decllist->nth(obj->decllist->count()-1)->endloc // XXX sometimes suffers from off by one error (one too early).  We work around this by sprinkling stray ; below.
                            );

      if(firstPairLoc.hasExactPosition() && secondPairLoc.hasExactPosition()) {
        UnboxedPairLoc firstUnboxedPairLoc(firstPairLoc);
        UnboxedPairLoc secondUnboxedPairLoc(secondPairLoc);
        std::string decl_prefix = patcher.getRange(firstUnboxedPairLoc);
        std::string decl_suffix = patcher.getRange(secondUnboxedPairLoc);
        patcher.insertBefore(tf->loc, decl_prefix + WRAP_PREFIX + pqname + decl_suffix + ";\n");
      } else {
        std::cerr << tf->loc << "WTF cannot patch " << pqname << " no exact position for declaration" << std::endl;
      }
#ifndef STRESS_TEST
    }
#endif
  } else if(deref_base->isD_pointer()) {
    // do nothing.  we don't wrap pointers yet.
  } else {
    std::cerr << tf->loc << " WTF is this function declaration base type " << deref_base->kindName() << std::endl;
  }
}

void WrapFunCall_ASTVisitor::postvisitExpression(Expression *obj) {
  if (!obj->isE_funCall()) return;

  const E_funCall* funCall= obj->asE_funCallC();
  if(funCall->func->isE_variable()) {
    const E_variable * var = funCall->func->asE_variableC();
    if(var->name->isPQ_name()) {
      std::string nm(var->name->asPQ_nameC()->name);
      if(wrapped_funcs.find(nm) != wrapped_funcs.end()) {
        PairLoc call_PairLoc(obj->loc, obj->endloc);
        if(!call_PairLoc.hasExactPosition()) {
          std::cerr << "D'oh! Found matching function call " << nm << " without exact location.  Not patching this sucker; good luck locating it." << std::endl;
        } else {
          patcher.insertBefore(obj->loc, WRAP_PREFIX);
        }
      }
    } else {
      std::cerr << "WTF type of a variable is a " << var->name->kindName() << "??" << std::endl;
    }
  }
  else if (funCall->func->isE_fieldAcc()) {
    // TODO
  }
  else if (funCall->func->isE_deref()) {
    // TODO
  }
  else {
    std::cerr << "WTF type of a func is a " << funCall->func->kindName() << "??" << std::endl;
  }
}


// **** Jimmy_ASTVisitor

class Jimmy_ASTVisitor : public OnlyDecltorsOfRealVars_ASTVisitor {
public:
  Patcher &patcher;

  Jimmy_ASTVisitor(Patcher &patcher0) : patcher(patcher0) {}

  virtual bool visitExpression(Expression *);

  bool subVisitS_return(S_return *);
  bool subVisitS_decl(S_decl *);
  bool subVisitE_variable(E_variable *);

  char *xformDeclarator(Declarator *);
};

// emit the free() expressions for all vars on the var stack of for
// all scopes on the scope stack
bool Jimmy_ASTVisitor::
subVisitS_return(S_return *obj) {
  return true;
}

bool Jimmy_ASTVisitor::subVisitS_decl(S_decl *obj) {
  return true;
}

// Note: caller is responsible for freeing the return value; note: we
// optimize the empty string by just returning NULL
char *Jimmy_ASTVisitor::
xformDeclarator(Declarator *obj) {
  return NULL;
}

bool Jimmy_ASTVisitor::
visitExpression(Expression *obj) {
  return true;
}

bool Jimmy_ASTVisitor::
subVisitE_variable(E_variable *evar) {
  return true;
}

// **** Xform

// print the locations of declarators and uses of stack allocated
// variables
void Xform::printStackAlloc_stage() {
  printStage("print stack-allocated vars");
  PrintStackAllocVars_ASTVisitor env;
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    TranslationUnit *unit = file2unit.get(file);

    printStart(file->name.c_str());
    unit->traverse(env.loweredVisitor);
    printStop();
  }
}

// print the locations of declarators and uses of stack allocated
// variables that have had their addr taken
void Xform::printStackAllocAddrTaken_stage() {
  printStage("print stack-allocated addr-taken vars");
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    TranslationUnit *unit = file2unit.get(file);

    // optimization: while a variable in one translation unit may have
    // its address taken in another, this cannot happen to
    // stack-allocated variables; if you wanted to find all variables
    // that had their address taken, you would need the linker
    // imitator
    SObjSet<Variable*> addrTaken;
    AddrTaken_ASTVisitor at_env(addrTaken);
    unit->traverse(at_env.loweredVisitor);

    PrintStackAllocAddrTakenVars_ASTVisitor env(addrTaken);
    printStart(file->name.c_str());
    unit->traverse(env.loweredVisitor);
    printStop();
  }
}

void Xform::heapifyStackAllocAddrTaken_stage(IssuesWarnings &warn) {
  printStage("heapify stack-allocated addr-taken vars");
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    if (globalLang.isCplusplus) {
      // some C++ concerns: objects that should use new instead of
      // malloc()
      throw UserError(USER_ERROR_ExitCode, "Can't xform heapify C++ yet.");
    }
    TranslationUnit *unit = file2unit.get(file);

    // optimization: while a variable in one translation unit may have
    // its address taken in another, this cannot happen to
    // stack-allocated variables; if you wanted to find all variables
    // that had their address taken, you would need the linker
    // imitator
    SObjSet<Variable*> addrTaken;
    AddrTaken_ASTVisitor at_env(addrTaken);
    unit->traverse(at_env.loweredVisitor);

    Patcher patcher(std::cout /*ostream for the diff*/, true /*recursive*/);
    HeapifyStackAllocAddrTakenVars_ASTVisitor env
      (warn, addrTaken, patcher, NULL /*root*/);
    unit->traverse(env.loweredVisitor);

    printStart(file->name.c_str());
    patcher.flush();
    printStop();
  }
}

void Xform::verifyCrossModuleParams_stage(IssuesWarnings &warn) {
  printStage("verify cross module params");
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    if (globalLang.isCplusplus) {
      // some C++ concerns: non-pointer objects that behave like
      // pointers because of operator overloading
      throw UserError(USER_ERROR_ExitCode, "Can't xform verify C++ yet.");
    }
    TranslationUnit *unit = file2unit.get(file);

    Patcher patcher(std::cout /*ostream for the diff*/, true /*recursive*/);
    VerifyCrossModuleParams_ASTVisitor env(warn, classFQName2Module, patcher);
    unit->traverse(env.loweredVisitor);

    printStart(file->name.c_str());
    patcher.flush();
    printStop();
  }
}

void Xform::localizeHeapAlloc_stage(IssuesWarnings &warn) {
  printStage("localize heap alloc");
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    if (globalLang.isCplusplus) {
      // some C++ concerns: new, delete
      throw UserError(USER_ERROR_ExitCode, "Can't xform localize C++ yet.");
    }
    TranslationUnit *unit = file2unit.get(file);

    Patcher patcher(std::cout /*ostream for the diff*/, true /*recursive*/);
    LocalizeHeapAlloc_ASTVisitor env(warn, classFQName2Module, patcher);
    unit->traverse(env.loweredVisitor);

    printStart(file->name.c_str());
    patcher.flush();
    printStop();
  }
}

void Xform::introFunCall_stage() {
  printStage("introduce function call");
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    if (globalLang.isCplusplus) {
      // some C++ concerns: implicit function calls
      throw UserError(USER_ERROR_ExitCode,
                      "Can't xform intro-fun-call C++ yet.");
    }
    TranslationUnit *unit = file2unit.get(file);

    Patcher patcher(std::cout /*ostream for the diff*/, true /*recursive*/);
    IntroFunCall_ASTVisitor env(patcher);
    unit->traverse(env.loweredVisitor);

    printStart(file->name.c_str());
    patcher.flush();
    printStop();
  }
}

void Xform::wrapFunCall_stage(const char * cnf_file) {
  printStage("introduce function call");
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    if (globalLang.isCplusplus) {
      // some C++ concerns: implicit function calls
      throw UserError(USER_ERROR_ExitCode,
                      "Can't xform intro-fun-call C++ yet.");
    }
    TranslationUnit *unit = file2unit.get(file);

    Patcher patcher(std::cout /*ostream for the diff*/, true /*recursive*/);
    WrapFunCall_ASTVisitor env(patcher, cnf_file);
    unit->traverse(env.loweredVisitor);

    printStart(file->name.c_str());
    patcher.flush();
    printStop();
  }
}


void Xform::jimmy_stage() {
  printStage("jimmy");
  foreachSourceFile {
    File *file = files.data();
    maybeSetInputLangFromSuffix(file);
    TranslationUnit *unit = file2unit.get(file);

    Patcher patcher(std::cout /*ostream for the diff*/, true /*recursive*/);
    Jimmy_ASTVisitor env(patcher);
    unit->traverse(env.loweredVisitor);

    printStart(file->name.c_str());
    patcher.flush();
    printStop();
  }
}
