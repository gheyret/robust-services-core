//==============================================================================
//
//  Editor.cpp
//
//  Copyright (C) 2013-2021  Greg Utas
//
//  This file is part of the Robust Services Core (RSC).
//
//  RSC is free software: you can redistribute it and/or modify it under the
//  terms of the GNU General Public License as published by the Free Software
//  Foundation, either version 3 of the License, or (at your option) any later
//  version.
//
//  RSC is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
//  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
//  details.
//
//  You should have received a copy of the GNU General Public License along
//  with RSC.  If not, see <http://www.gnu.org/licenses/>.
//
#include "Editor.h"
#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iosfwd>
#include <iterator>
#include <list>
#include <sstream>
#include "CliThread.h"
#include "CodeCoverage.h"
#include "CodeFile.h"
#include "CxxArea.h"
#include "CxxDirective.h"
#include "CxxLocation.h"
#include "CxxNamed.h"
#include "CxxScope.h"
#include "CxxScoped.h"
#include "CxxString.h"
#include "CxxSymbols.h"
#include "CxxToken.h"
#include "CxxVector.h"
#include "Debug.h"
#include "Duration.h"
#include "Formatters.h"
#include "Library.h"
#include "LibraryItem.h"
#include "NbCliParms.h"
#include "Singleton.h"
#include "SysFile.h"
#include "ThisThread.h"

using std::ostream;
using std::string;
using namespace NodeBase;

//------------------------------------------------------------------------------

namespace CodeTools
{
//  The editors that have modified their original code.  This allows multiple
//  files to be changed (e.g. when a fix requires changes in both a function's
//  declaration and definition).  After all changes needed for a fix have been
//  made, all modified files can be committed.
//
std::set< Editor* > Editors_ = std::set< Editor* >();

//  The number of files committed so far.
//
size_t Commits_ = 0;

//------------------------------------------------------------------------------
//
//  Return codes from editor functions.
//
const word EditAbort = -2;     // stream error or fix not implemented
const word EditFailed = -1;    // could not fix; write info to CLI
const word EditSucceeded = 0;  // warning fixed; write info to CLI
const word EditContinue = 0;   // continue with edit
const word EditCompleted = 1;  // warning closed; don't write info to CLI

//  The last result.  It, and the two functions that manage it, are similar
//  to the SetLastError/GetLastError interface.  Based on the return code,
//  Expl_ should contain
//  o EditFailure or EditAbort: an explanation of why the edit failed
//  o EditSucceeded: an edited line of code (or an empty string after deletion)
//  o EditContinue: an empty string, because there is nothing to report
//  o EditCompleted: an empty string, because all results have been reported
//
string Expl_;

//  Sets Expl_ to EXPL, adding a CRLF if EXPL doesn't have one.
//
void SetExpl(const string& expl);

//  Returns Expl_ after clearing it.
//
string GetExpl();

//------------------------------------------------------------------------------
//
//  Strings for user interaction.
//
fixed_string FixPrompt = "Fix?";
fixed_string YNSQHelp = "Enter y(yes) n(no) s(skip file) q(quit): ";
fixed_string YNSQChars = "ynsq";
fixed_string FixSkipped = "This fix will be skipped.";
fixed_string ItemDeleted = "The item associated with this warning was deleted.";
fixed_string NotImplemented = "Fixing this warning is not yet supported.";
fixed_string UnspecifiedFailure = "Internal error. Edit failed.";

fixed_string AccessPrompt = "Enter 0=skip 1=public 2=protected 3=private: ";
fixed_string ArgPrompt = "Choose the argument's name. ";
fixed_string DefnPrompt = "0=skip 1=default 2=delete: ";
fixed_string FilePrompt = "Enter the filename in which to define ";
fixed_string ShellPrompt = "Do you want to create a shell for the definition?";
fixed_string SuffixPrompt = "Enter a suffix for the name: ";
fixed_string VirtualPrompt = "Enter 0=skip 1=virtual 2=non-virtual: ";
fixed_string CommitFailedPrompt = "Do you want to retry?";

fixed_string ClassInstantiated = "Objects of this class are created, "
                                 "so its constructor must remain public.";

//==============================================================================
//
//  Where to add a blank line when declaring a C++ item.
//
enum BlankLocation
{
   BlankNone,
   BlankAbove,
   BlankBelow
};

//  Attributes when declaring a C++ item.
//
struct ItemDeclAttrs
{
   //  Initializes the attributes for an item of type T, with access control A.
   //
   ItemDeclAttrs(Cxx::ItemType t, Cxx::Access a, FunctionRole r = FuncOther);

   //  Initializes the attributes for ITEM.
   //
   explicit ItemDeclAttrs(const CxxToken* item);

   //  Returns the order, within a class, where the item should be declared.
   //
   size_t CalcDeclOrder() const;

   //  The following are provided as inputs.
   //
   const Cxx::ItemType type_;  // type of item being declared
   Cxx::Access access_;        // desired access control
   FunctionRole role_;         // if a function, the type being added
   bool over_;                 // set if a function is an override

   //  The following are calculated internally.
   //
   bool isstruct_;         // set if item belongs to a struct
   bool oper_;             // set for an operator
   bool virt_;             // set to make a function virtual
   bool deleted_;          // set to define a function as deleted
   bool shell_;            // set to create a shell for defining a function
   bool stat_;             // set for a static function or data
   bool thisctrl_;         // set to insert access control before
   Cxx::Access nextctrl_;  // set to insert access control after
   size_t pos_;            // position for insertion
   size_t indent_;         // number of spaces for indentation
   BlankLocation blank_;   // where to insert a blank line
   bool comment_;          // set to include a comment
};

//------------------------------------------------------------------------------

ItemDeclAttrs::ItemDeclAttrs(Cxx::ItemType t, Cxx::Access a, FunctionRole r) :
   type_(t),
   access_(a),
   role_(r),
   over_(false),
   isstruct_(false),
   oper_(false),
   virt_(false),
   deleted_(false),
   shell_(false),
   stat_(false),
   thisctrl_(false),
   nextctrl_(Cxx::Access_N),
   pos_(string::npos),
   indent_(0),
   blank_(BlankNone),
   comment_(false)
{
   Debug::ft("ItemDeclAttrs.ctor(type)");
}

//------------------------------------------------------------------------------

ItemDeclAttrs::ItemDeclAttrs(const CxxToken* item) :
   type_(item->Type()),
   access_(item->GetAccess()),
   role_(FuncOther),
   over_(false),
   isstruct_(false),
   oper_(false),
   virt_(false),
   deleted_(false),
   shell_(false),
   stat_(false),
   thisctrl_(false),
   nextctrl_(Cxx::Access_N),
   pos_(string::npos),
   indent_(0),
   blank_(BlankNone),
   comment_(false)
{
   Debug::ft("ItemDeclAttrs.ctor(item)");

   switch(type_)
   {
   case Cxx::Function:
   {
      auto func = static_cast< const Function* >(item);
      role_ = func->FuncRole();
      over_ = func->IsOverride();
      oper_ = (func->FuncType() == FuncOperator);
      virt_ = func->IsVirtual();
      //  [[fallthrough]]
   }
   case Cxx::Data:
      stat_ = item->IsStatic();
      break;
   }

   auto cls = item->GetClass();
   if(cls != nullptr) isstruct_ = (cls->GetClassTag() != Cxx::ClassType);

   auto& editor = item->GetFile()->GetEditor();
   size_t begin, end;
   if(item->GetSpan2(begin, end))
   {
      indent_ = editor.LineFindFirst(begin) - editor.CurrBegin(begin);
   }
}

//------------------------------------------------------------------------------

fn_name ItemDeclAttrs_CalcDeclOrder = "ItemDeclAttrs.CalcDeclOrder";

size_t ItemDeclAttrs::CalcDeclOrder() const
{
   Debug::ft(ItemDeclAttrs_CalcDeclOrder);

   //  Items have to be declared in some order, so this tries to organize
   //  them in a consistent way. The first thing that determines order is
   //  the item's access control.
   //
   size_t order = 0;

   switch(access_)
   {
   case Cxx::Public:
      order = 0;
      break;
   case Cxx::Protected:
      order = 20;
      break;
   default:
      order = 40;
   }

   switch(type_)
   {
   case Cxx::Friend:
      return order + 3;
   case Cxx::Forward:
      return order + 4;
   case Cxx::Enum:
      return order + 5;
   case Cxx::Typedef:
      return order + 6;
   case Cxx::Class:
      return order + 7;

   case Cxx::Function:
   {
      switch(role_)
      {
      case PureCtor:
         return order + 8;
      case PureDtor:
         return order + 9;
      case CopyCtor:
         return order + 10;
      case MoveCtor:
         return order + 11;
      case CopyOper:
         return order + 12;
      case MoveOper:
         return order + 13;
      default:
         if(oper_) return order + 14;
         if(virt_) return order + 16;
         if(over_) return order + 17;
         return order + 15;
      }
      break;
   }

   case Cxx::Data:
      if(isstruct_)
      {
         //  If a struct defines its data first, this will prevent functions
         //  from being added before its data.
         //
         if(stat_) return order + 2;
         return order + 1;
      }
      else
      {
         if(stat_) return order + 19;
         return order + 18;
      }
   }

   Debug::SwLog(ItemDeclAttrs_CalcDeclOrder, "unexpected item type", type_);
   return order;
}

//==============================================================================
//
//  How an item's definition is separated from other code.
//
enum ItemOffset
{
   OffsetNone,   // not offset
   OffsetBlank,  // blank line
   OffsetRule    // blank line and rule
};

struct ItemOffsets
{
   ItemOffsets() : above_(OffsetNone), below_(OffsetNone) { }

   ItemOffset above_;  // offset above the item
   ItemOffset below_;  // offset below the item
};

//------------------------------------------------------------------------------
//
//  Attributes when inserting an item definition.
//
struct ItemDefnAttrs
{
   explicit ItemDefnAttrs(FunctionRole role);
   explicit ItemDefnAttrs(const Function* func);

   const FunctionRole role_;  // type of function being inserted
   size_t pos_;               // insertion position
   ItemOffsets offsets_;      // how to offset item
};

ItemDefnAttrs::ItemDefnAttrs(FunctionRole role) :
   role_(role),
   pos_(string::npos)
{
   Debug::ft("ItemDefnAttrs.ctor(role)");
}

ItemDefnAttrs::ItemDefnAttrs(const Function* func) :
   role_(func->FuncRole()),
   pos_(string::npos)
{
   Debug::ft("ItemDefnAttrs.ctor(func)");
}

//==============================================================================
//
//  Returns true if ITEM1 and ITEM2 appear in the same statement: specifically,
//  if a semicolon does not appear between their positions.
//
bool AreInSameStatement(const CxxToken* item1, const CxxToken* item2)
{
   Debug::ft("CodeTools.AreInSameStatement");

   if((item1 != nullptr) && (item2 != nullptr))
   {
      auto file1 = item1->GetFile();
      auto file2 = item2->GetFile();
      if(file1 != file2) return false;

      auto pos1 = item1->GetPos();
      auto pos2 = item2->GetPos();
      if(pos1 == pos2) return true;

      auto& editor = file1->GetEditor();
      auto begin = (pos1 < pos2 ? pos1 : pos2);
      auto end = (pos1 > pos2 ? pos1 : pos2);
      return (editor.FindFirstOf(";", begin) > end);
   }

   return false;
}

//------------------------------------------------------------------------------
//
//  Asks the user to choose declName or defnName as an argument's name.
//
string ChooseArgumentName
   (CliThread& cli, const string& declName, const string& defnName)
{
   Debug::ft("CodeTools.ChooseArgumentName");

   std::ostringstream stream;
   stream << spaces(2) << ArgPrompt << CRLF;
   stream << spaces(4) << "1=" << QUOTE << declName << QUOTE << SPACE;
   stream << spaces(4) << "2=" << QUOTE << defnName << QUOTE << ": ";
   auto choice = cli.IntPrompt(stream.str(), 1, 2);
   return (choice == 1 ? declName : defnName);
}

//------------------------------------------------------------------------------
//
//  Asks the user to determine the attributes for a destructor that is
//  currently non-virtual.
//
word ChooseDtorAttributes(CliThread& cli, ItemDeclAttrs& attrs)
{
   Debug::ft("CodeTools.ChooseDtorAttributes");

   std::ostringstream stream;
   stream << spaces(2) << AccessPrompt;
   auto response = cli.IntPrompt(stream.str(), 0, 3);

   auto access = Cxx::Access_N;
   auto virt = false;

   switch(response)
   {
   case 1:
      access = Cxx::Public;
      break;
   case 2:
      access = Cxx::Protected;
      break;
   case 3:
      access = Cxx::Private;
      break;
   default:
      return EditFailed;
   }

   stream.str(EMPTY_STR);
   stream << spaces(2) << VirtualPrompt;
   response = cli.IntPrompt(stream.str(), 0, 2);

   switch(response)
   {
   case 1:
      virt = true;
      break;
   case 2:
      virt = false;
      break;
   default:
      return EditFailed;
   }

   if((attrs.access_ == access) && (attrs.virt_ == virt)) return EditCompleted;
   attrs.access_ = access;
   attrs.virt_ = virt;
   return EditSucceeded;
}

//------------------------------------------------------------------------------
//
//  Creates a comment for the special member function that will be added
//  to CLS according to ATTRS.
//
string CreateSpecialFunctionComment
   (const Class* cls, const ItemDeclAttrs& attrs)
{
   Debug::ft("CodeTools.CreateSpecialFunctionComment");

   string comment;

   if(attrs.deleted_)
   {
      //  Currently occurs only for a copy constructor/operator.
      //
      switch(attrs.role_)
      {
      case CopyCtor:
      case MoveCtor:
         comment = "Deleted to prohibit copying";
         break;
      case CopyOper:
      case MoveOper:
         comment = "Deleted to prohibit copy assignment";
         break;
      case PureCtor:
      case PureDtor:
         comment = "Deleted because this class only has static members";
         break;
      default:
         comment = "[Add comment.]";
      }
   }
   else if(attrs.virt_)
   {
      //  For a base class destructor.
      //
      if(cls->IsSingletonBase())
         comment = "Protected because subclasses should be singletons";
      else
         comment = "Virtual to allow subclassing";
   }
   else if(attrs.access_ == Cxx::Protected)
   {
      //  For a base class (other than the destructor).
      //
      comment = "Protected because this class is virtual";
   }
   else if(attrs.access_ == Cxx::Private)
   {
      //  For a singleton's constructor or destructor.
      //
      comment = "Private because this is a singleton";
   }
   else if(attrs.role_ == PureDtor)
   {
      //  For a leaf class destructor.
      //
      comment = "Not subclassed";
   }
   else
   {
      //  None of the above applies.
      //
      std::ostringstream stream;
      stream << attrs.role_;
      comment = stream.str();
      comment.front() = toupper(comment.front());
   }

   comment.push_back('.');
   return comment;
}

//------------------------------------------------------------------------------
//
//  Returns an unquoted string (FLIT) and fn_name identifier (FVAR) that are
//  suitable for invoking Debug::ft.
//
void DebugFtNames(const Function* func, string& flit, string& fvar)
{
   Debug::ft("CodeTools.DebugFtNames");

   //  Get the function's name and the area in which it appears.
   //
   auto sname = func->GetArea()->Name();
   auto fname = func->DebugName();

   flit = sname;
   if(!sname.empty()) flit.push_back('.');
   flit.append(fname);

   fvar = sname;
   if(!fvar.empty()) fvar.push_back('_');

   if(func->FuncType() == FuncOperator)
   {
      //  Something like "class_operator=" won't pass as an identifier, so use
      //  "class_operatorN", where N is the integer value of the Operator enum.
      //
      auto oper = CxxOp::NameToOperator(fname);
      fname.erase(strlen(OPERATOR_STR));
      fname.append(std::to_string(oper));
   }

   fvar.append(fname);
}

//------------------------------------------------------------------------------
//
//  Sets FNAME to FLIT, the argument for Debug::ft.  If it is already in use,
//  prompts the user for a suffix to make it unique.  Returns false if FNAME
//  is not unique and the user did not provide a satisfactory suffix, else
//  returns true after enclosing FNAME in quotes.
//
bool EnsureUniqueDebugFtName(CliThread& cli, const string& flit, string& fname)
{
   Debug::ft("CodeTools.EnsureUniqueDebugFtName");

   auto cover = Singleton< CodeCoverage >::Instance();
   fname = flit;

   while(cover->Defined(fname))
   {
      std::ostringstream stream;
      stream << spaces(2) << fname << " is already in use. " << SuffixPrompt;
      auto suffix = cli.StrPrompt(stream.str());
      if(suffix.empty()) return false;
      fname = flit + '(' + suffix + ')';
   }

   fname.insert(0, 1, QUOTE);
   fname.push_back(QUOTE);
   return true;
}

//------------------------------------------------------------------------------
//
//  Returns the file where a class's new function should be defined.  If all of
//  the its functions are implemented in the same file, returns that file, else
//  asks the user to specify the file.
//
CodeFile* FindFuncDefnFile(CliThread& cli, const Class* cls, const string& name)
{
   Debug::ft("CodeTools.FindFuncDefnFile");

   std::set< CodeFile* > impls;
   auto funcs = cls->Funcs();

   for(auto f = funcs->cbegin(); f != funcs->cend(); ++f)
   {
      auto file = (*f)->GetDefnFile();
      if((file != nullptr) && file->IsCpp()) impls.insert(file);
   }

   CodeFile* file = (impls.size() == 1 ? *impls.cbegin() : nullptr);

   while(file == nullptr)
   {
      std::ostringstream prompt;
      prompt << spaces(2) << FilePrompt << CRLF;
      prompt << spaces(4) << cls->Name() << SCOPE_STR << name;
      prompt << " ('s' to skip this item): ";
      auto fileName = cli.StrPrompt(prompt.str());
      if(fileName == "s") return nullptr;

      file = Singleton< Library >::Instance()->FindFile(fileName);
      if(file == nullptr)
      {
         cli.Inform("  That file is not in the code library.");
      }
   }

   return file;
}

//------------------------------------------------------------------------------
//
//  Updates BEGIN and END, where ITEM begins and ends.  BEGIN includes any
//  comment that immediately precedes ITEM.  Returns false if ITEM could
//  not be located.
//
bool GetCommentedRange(const CxxToken* item, size_t& begin, size_t& end)
{
   Debug::ft("CodeTools.GetCommentedRange");

   if(!item->GetSpan2(begin, end)) return false;

   auto& editor = item->GetFile()->GetEditor();
   begin = editor.IntroStart(begin, (item->Type() == Cxx::Function));
   return true;
}

//------------------------------------------------------------------------------
//
//  Adds DATA and its separate definition, if it exists, to DATAS.
//
void GetDatas(const Data* data, std::vector< const Data* >& datas)
{
   Debug::ft("CodeTools.GetDatas");

   auto defn = static_cast< const Data* >(data->GetMate());
   if(defn != nullptr) datas.push_back(defn);
   datas.push_back(data);
}

//------------------------------------------------------------------------------

string GetExpl()
{
   Debug::ft("CodeTools.GetExpl");

   auto expl = Expl_;
   Expl_.clear();
   return expl;
}

//------------------------------------------------------------------------------
//
//  Adds FUNC and its overrides to FUNCS.
//
void GetOverrides(const Function* func, std::vector< const Function* >& funcs)
{
   Debug::ft("CodeTools.GetOverrides");

   auto& overs = func->GetOverrides();

   for(auto f = overs.cbegin(); f != overs.cend(); ++f)
   {
      GetOverrides(*f, funcs);
   }

   auto defn = static_cast< const Function* >(func->GetMate());
   if((defn != nullptr) && (defn != func)) funcs.push_back(defn);

   funcs.push_back(func);
}

//------------------------------------------------------------------------------
//
//  Adds all references to DATA to ITEMS, excluding DATA and its definition.
//
void GetReferences(const Data* data, std::vector< const CxxNamed* >& items)
{
   Debug::ft("CodeTools.GetReferences");

   auto xref = data->Xref();
   auto mate = data->GetMate();

   for(auto r = xref->cbegin(); r != xref->cend(); ++r)
   {
      if(AreInSameStatement(data, *r)) continue;
      if(AreInSameStatement(mate, *r)) continue;
      items.push_back(*r);
   }
}

//------------------------------------------------------------------------------
//
//  Returns true if ITEM, whose CODE is provided, is a non-trival inline.
//  To be an inline, it must be a function without a separate definition, but
//  it must have an implementation (as opposed to being deleted or defaulted).
//  To be non-trivial, its code must span at least three lines.
//
bool IsNonTrivialInline(const CxxNamed* item, const string& code)
{
   Debug::ft("CodeTools.IsNonTrivialInline");

   if(item->Type() != Cxx::Function) return false;
   auto func = static_cast< const Function* >(item);

   if(func->GetDefn() != func) return false;
   auto impl = func->GetImpl();
   if(impl == nullptr) return false;

   for(size_t i = 0, n = 0; i < code.size(); ++i)
   {
      if(code[i] == CRLF)
      {
         if(++n > 2) return true;
      }
   }

   return false;
}

//------------------------------------------------------------------------------
//
//  Returns true if a comment precedes ITEM.
//
bool ItemIsCommented(const CxxToken* item)
{
   Debug::ft("CodeTools.ItemIsCommented");

   size_t begin, end;
   GetCommentedRange(item, begin, end);
   auto& editor = item->GetFile()->GetEditor();
   return !editor.OnSameLine(begin, item->GetPos());
}

//------------------------------------------------------------------------------
//
//  Returns true if an item between BEGIN and END references ITEM.
//
bool ItemIsUsedBetween(const CxxNamed* item, size_t begin, size_t end)
{
   Debug::ft("CodeTools.ItemIsUsedBetween");

   auto xref = item->Xref();
   if(xref == nullptr) return false;

   for(auto r = xref->cbegin(); r != xref->cend(); ++r)
   {
      auto pos = (*r)->GetPos();
      if((pos >= begin) && (pos <= end)) return true;
   }

   return false;
}

//------------------------------------------------------------------------------
//
//  Sets Expl_ to "TEXT not found."  If QUOTES is set, TEXT is enclosed in
//  quotes.  Returns 0.
//
word NotFound(fixed_string text, bool quotes = false)
{
   Debug::ft("CodeTools.NotFound");

   string expl;
   if(quotes) expl = QUOTE;
   expl += text;
   if(quotes) expl.push_back(QUOTE);
   expl += " not found.";
   SetExpl(expl);
   return EditFailed;
}

//------------------------------------------------------------------------------

word Report(fixed_string text, word rc = EditFailed)
{
   Debug::ft("CodeTools.Report");

   SetExpl(text);
   return rc;
}

//------------------------------------------------------------------------------

word Report(const std::ostringstream& stream, word rc = EditFailed)
{
   Debug::ft("CodeTools.Report(stream)");

   SetExpl(stream.str());
   return rc;
}

//------------------------------------------------------------------------------

void SetExpl(const string& expl)
{
   Debug::ft("CodeTools.SetExpl");

   Expl_ = expl;
   if(!expl.empty() && expl.back() != CRLF) Expl_.push_back(CRLF);
}

//------------------------------------------------------------------------------

string strCode(const string& code, size_t level)
{
   return spaces(level * IndentSize()) + code + CRLF;
}

//------------------------------------------------------------------------------
//
//  Returns TEXT, prefixed by "//  " and indented with INDENT leading spaces.
//
string strComment(const string& text, size_t indent)
{
   auto comment = spaces(indent) + "//";
   if(!text.empty()) comment += spaces(2) + text;
   comment.push_back(CRLF);
   return comment;
}

//------------------------------------------------------------------------------
//
//  Invoked when fixing a warning still needs to be implemented.  This
//  normally doesn't occur because WarningAttrs.fixable should be false.
//
word Unimplemented()
{
   Debug::ft("CodeTools.Unimplemented");

   return Report(NotImplemented);
}

//==============================================================================

Editor::Editor() :
   file_(nullptr),
   sorted_(false),
   aliased_(false),
   lastCut_(string::npos)
{
   Debug::ft("Editor.ctor");
}

//------------------------------------------------------------------------------

bool Editor::AdjustHorizontally(size_t pos, size_t len, const string& spacing)
{
   Debug::ft("Editor.AdjustHorizontally");

   auto changed = false;
   auto prev = pos - 1;
   auto next = pos + len;

   if(spacing[0] == Spacing::NoGap)
   {
      auto info = GetLineInfo(pos);
      auto begin = LineRfindNonBlank(prev);
      if(begin < info->depth) begin = info->depth;

      if(begin < prev)
      {
         auto count = prev - begin;
         Erase(begin + 1, count);
         next -= count;
         changed = true;
      }
   }
   else if(spacing[0] == Spacing::Gap)
   {
      if(!IsBlank(source_[prev]))
      {
         Insert(pos, SPACE_STR);
         ++next;
         changed = true;
      }
   }

   if(spacing[1] == Spacing::NoGap)
   {
      auto end = LineFindNonBlank(next);
      if(end > next)
      {
         Erase(next, end - next);
         changed = true;
      }
   }
   else if(spacing[1] == Spacing::Gap)
   {
      if(!IsBlank(source_[next]))
      {
         Insert(next, SPACE_STR);
         changed = true;
      }
   }

   return changed;
}

//------------------------------------------------------------------------------

word Editor::AdjustIndentation(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustIndentation");

   //  Adjust the code's indentation to match its lexical depth.
   //
   auto pos = log.loc_.GetPos();
   return Indent(pos);
}

//------------------------------------------------------------------------------

word Editor::AdjustOperator(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustOperator");

   auto oper = static_cast< const Operation* >(log.item_);
   auto& attrs = CxxOp::Attrs[oper->Op()];

   if(AdjustHorizontally(oper->GetPos(), attrs.symbol.size(), attrs.spacing))
      return Changed(oper->GetPos());
   return NotFound("operator adjustment");
}

//------------------------------------------------------------------------------

word Editor::AdjustPunctuation(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustPunctuation");

   if(log.info_.size() != 2) return NotFound("log information");
   if(AdjustHorizontally(log.Pos(), 1, log.info_)) return Changed(log.Pos());
   return NotFound("punctuation adjustment");
}

//------------------------------------------------------------------------------

word Editor::AdjustTags(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustTags");

   //  A pointer tag should not be preceded by a space.  It either adheres to
   //  its type or "const".  The same is true for a reference tag, which can
   //  also adhere to a pointer tag.  Even if there is more than one detached
   //  pointer tag, only one log is generated, so fix them all.
   //
   auto tag = (log.warning_ == PtrTagDetached ? '*' : '&');
   auto stop = CurrEnd(log.Pos());
   auto changed = false;

   for(auto pos = source_.find(tag, log.Pos()); pos < stop;
      pos = source_.find(tag, pos + 1))
   {
      if(IsBlank(source_[pos - 1]))
      {
         auto prev = RfindNonBlank(pos - 1);
         auto count = pos - prev - 1;
         Erase(prev + 1, count);
         pos -= count;

         //  If the character after the tag is the beginning of an identifier,
         //  insert a space.
         //
         if(ValidFirstChars.find(source_[pos + 1]) != string::npos)
         {
            Insert(pos + 1, SPACE_STR);
         }

         changed = true;
         break;
      }
   }

   if(changed) return Changed(log.Pos());

   string target = "Detached ";
   target.push_back(tag);
   target.push_back(SPACE);
   return NotFound(target.c_str());
}

//------------------------------------------------------------------------------

word Editor::AdjustVertically()
{
   Debug::ft("Editor.AdjustVertically");

   auto done = false;

   for(auto n = 1; !done && (n <= 16); ++n)
   {
      auto actions = CheckVerticalSpacing();
      done = true;

      for(size_t pos = 0, i = 0; pos != string::npos; ++i)
      {
         switch(actions[i])
         {
         case LineOK:
            pos = NextBegin(pos);
            break;

         case InsertBlank:
            InsertLine(pos, EMPTY_STR);
            pos = NextBegin(pos);
            pos = NextBegin(pos);
            done = false;
            break;

         case ChangeToEmptyComment:
            Insert(pos, COMMENT_STR);
            pos = NextBegin(pos);
            done = false;
            break;

         case DeleteLine:
            EraseLine(pos);
            done = false;
            break;
         }
      }
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::ChangeAccess(const CodeWarning& log, Cxx::Access acc)
{
   Debug::ft("Editor.ChangeAccess(log)");

   ItemDeclAttrs attrs(log.item_);
   attrs.access_ = acc;
   return ChangeAccess(log.item_, attrs);
}

//------------------------------------------------------------------------------

word Editor::ChangeAccess(const CxxToken* item, ItemDeclAttrs& attrs)
{
   Debug::ft("Editor.ChangeAccess(item)");

   //  Move the item's declaration and update its access control.  Its
   //  existing comment, if any, will also move, so don't insert one.
   //
   string code;
   auto from = CutCode(item, code);
   auto rc = FindItemDeclLoc(item->GetClass(), item->Name(), attrs);

   if(rc != EditContinue)
   {
      Paste(from, code, from);
      return Report(UnspecifiedFailure, rc);
   }

   attrs.comment_ = false;
   InsertBelowItemDecl(attrs);
   Paste(attrs.pos_, code, from);
   InsertAboveItemDecl(attrs, EMPTY_STR);
   const_cast< CxxToken* >(item)->SetAccess(attrs.access_);
   return Changed(item->GetPos());
}

//------------------------------------------------------------------------------

word Editor::ChangeAssignmentToCtorCall(const CodeWarning& log)  //u
{
   Debug::ft("Editor.ChangeAssignmentToCtorCall");

   //  Change ["const"] <type> <name> = <class> "(" [<args>] ");"
   //      to ["const"] <class> <name> "(" [<args>] ");"
   //
   //  Start by erasing ["const"] <type>, leaving a space before <name>.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Initialization statement");
   auto first = LineFindFirst(begin);
   if(first == string::npos) return NotFound("Start of code");
   auto name = FindWord(first, log.item_->Name());
   if(name == string::npos) return NotFound("Variable name");
   Erase(first, name - first - 1);

   //  Erase <class>.
   //
   auto eq = FindFirstOf("=", first);
   if(eq == string::npos) return NotFound("Assignment operator");
   auto cbegin = FindNonBlank(eq + 1);
   if(cbegin == string::npos) return NotFound("Start of class name");
   auto lpar = FindFirstOf("(", eq);
   if(lpar == string::npos) return NotFound("Left parenthesis");
   auto cend = RfindNonBlank(lpar - 1);
   if(cend == string::npos) return NotFound("End of class name");
   auto cname = source_.substr(cbegin, cend - cbegin + 1);
   Erase(cbegin, cend - cbegin + 1);

   //  Paste <class> before <name> and make it const if necessary.
   //
   Paste(first, cname, cbegin);
   if(log.item_->IsConst()) first = Insert(first, "const ");

   //  Remove the '=' and the spaces around it.
   //
   eq = FindFirstOf("=", first);
   if(eq == string::npos) return NotFound("Assignment operator");
   auto left = RfindNonBlank(eq - 1);
   auto right = FindNonBlank(eq + 1);
   Erase(left + 1, right - left - 1);

   //  If there are no arguments, remove the parentheses.
   //
   lpar = FindFirstOf("(", left);
   if(lpar == string::npos) return NotFound("Left parenthesis");
   auto rpar = FindClosing('(', ')', lpar + 1);
   if(rpar == string::npos) return NotFound("Right parenthesis");
   if(source_.find_first_not_of(WhitespaceChars, lpar + 1) == rpar)
   {
      Erase(lpar, rpar - lpar + 1);
   }

   //  If the code spanned two lines, it may be possible to remove the
   //  line break.
   //
   auto semi = FindFirstOf(";", lpar - 1);
   if(!OnSameLine(begin, semi)) EraseLineBreak(begin);
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::ChangeClassToNamespace(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeClassToNamespace");

   //  Replace "class" with "namespace" and "static" with "extern" (for data)
   //  or nothing (for functions).  Delete things that are no longer needed:
   //  base class, access controls, special member functions, and closing ';'.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeClassToStruct(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeClassToStruct");

   //  Start by changing the class's forward declarations.
   //
   ChangeForwards(log.item_, Cxx::ClassType, Cxx::StructType);

   //  Look for the class's name and then back up to "class".
   //
   auto pos = Rfind(log.item_->GetPos(), CLASS_STR);
   if(pos == string::npos) return NotFound(CLASS_STR, true);
   Replace(pos, strlen(CLASS_STR), STRUCT_STR);

   //  If the class began with a "public:" access control, erase it.
   //
   auto left = Find(pos, "{");
   if(left == string::npos) return NotFound("Left brace");
   auto access = FindWord(left + 1, PUBLIC_STR);
   if(access != string::npos)
   {
      auto colon = FindNonBlank(access + strlen(PUBLIC_STR));
      Erase(colon, 1);
      Erase(access, strlen(PUBLIC_STR));
      if(IsBlankLine(access)) EraseLine(access);
   }

   ((Class*) log.item_)->SetClassTag(Cxx::StructType);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::Changed()
{
   Debug::ft("Editor.Changed");

   Editors_.insert(this);
   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::Changed(size_t pos)
{
   Debug::ft("Editor.Changed(pos)");

   auto code = GetCode(pos);
   SetExpl(IsBlankLine(pos) ? EMPTY_STR : code);
   Editors_.insert(this);
   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::ChangeDataToFree(const CxxNamed* item, const Data* data)
{
   Debug::ft("Editor.ChangeDataToFree");

   //  This is invoked to remove static member data from its class and insert
   //  it into a namespace.  This only occurs when the data is used in a single
   //  .cpp (which might also be where the class is defined).  It is invoked
   //  in three ways, and ITEM is nullptr except in the first of these:
   //     1. On ITEM, a reference to DATA
   //     2. On DATA's declaration.
   //     3. On DATA's definition, if distinct.
   //  The first simply has to remove the class qualifier used to reference
   //  the data.  The other two are far more complicated, and the details
   //  depend on whether there is a separate definition.  In the end, code
   //  in the declaration and/or definition will be cut, fragments possibly
   //  combined, and pasted at the top of the .cpp.  The original items will
   //  then be deleted and the new code incrementally compiled.  This is a
   //  far more generic solution than trying to update all affected objects
   //  to account for the data moving from a class to a namespace.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

void Editor::ChangeForwards
   (const CxxToken* item, Cxx::ClassTag from, Cxx::ClassTag to)
{
   Debug::ft("Editor.ChangeForwards");

   auto prev = (from == Cxx::ClassType ? CLASS_STR : STRUCT_STR);
   auto next = (from == Cxx::ClassType ? CLASS_STR : STRUCT_STR);
   SymbolVector forwards;
   auto syms = Singleton< CxxSymbols >::Instance();

   syms->FindItems(item->Name(), FORW_MASK | FRIEND_MASK, forwards);

   for(auto f = forwards.cbegin(); f != forwards.cend(); ++f)
   {
      if(!(*f)->IsInternal() && ((*f)->Referent() == item))
      {
         auto& editor = (*f)->GetFile()->GetEditor();
         auto pos = (*f)->GetPos();
         auto cpos = editor.Find(pos, prev);
         if(cpos == string::npos) continue;
         editor.Replace(pos, strlen(prev), next);
         static_cast< Forward* >(*f)->SetClassTag(to);
      }
   }
}

//------------------------------------------------------------------------------

word Editor::ChangeFunctionToFree(const Function* func)
{
   Debug::ft("Editor.ChangeFunctionToFree");

   //  o If the function is invoked externally, move its declaration to after
   //    its class, into the enclosing namespace, else just erase it.
   //  o In the definition, replace the class name with the namespace in the
   //    fn_name or Debug::ft string literal.  If it uses any static items
   //    from the class, prefix the class name to those items.
   //  o Move the definition to the correct location.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeFunctionToMember(const Function* func, word offset)
{
   Debug::ft("Editor.ChangeFunctionToMember");

   //  o Declare the function in the class associated with the argument at
   //    OFFSET, removing that argument.
   //  o Define the function in the correct location, changing the argument
   //    at OFFSET to "this".
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeInvokerToFree(const Function* func)
{
   Debug::ft("Editor.ChangeInvokerToFree");

   //  Change invokers of this function to invoke it directly instead of
   //  through its class.  An invoker not in the same namespace may have
   //  to prefix the function's namespace.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeInvokerToMember(const Function* func, word offset)
{
   Debug::ft("Editor.ChangeInvokerToMember");

   //  Change invokers of this function to invoke it through the argument at
   //  OFFSET instead of directly.  An invoker may need to #include the class's
   //  header.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeOperator(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeOperator");

   //  This fixes two different warnings:
   //  o StaticFunctionViaMember: change . or -> to :: and what precedes
   //    the operator to the name of the function's class.
   //  o BitwiseOperatorOnBoolean: replace | with || or & with &&.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeSpecialFunction(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.ChangeSpecialFunction");

   //  This is logged on a class when the function doesn't yet exist.
   //
   if(log.item_->Type() == Cxx::Class)
   {
      return InsertSpecialFunctions(cli, log.item_);
   }

   //  The function already exists, so its access control needs to change.
   //
   auto func = static_cast< const Function* >(log.item_);
   auto cls = func->GetClass();
   ItemDeclAttrs attrs(func);
   word rc = EditFailed;

   switch(log.warning_)
   {
   case PublicConstructor:
      if(cls->WasCreated(false)) return Report(ClassInstantiated);
      attrs.access_ = Cxx::Protected;
      break;

   case NonVirtualDestructor:
      switch(ChooseDtorAttributes(cli, attrs))
      {
      case EditCompleted:
         return Report("Destructor remains unchanged.", EditCompleted);
      case EditFailed:
         return Report(FixSkipped);
      }

      if(attrs.virt_ != func->IsVirtual())
      {
         if(attrs.virt_)
            rc = TagAsVirtual(log);
         else
            rc = EraseVirtualTag(log);
         if(rc != EditSucceeded) return rc;
      }
      break;

   case ConstructorNotPrivate:
   case DestructorNotPrivate:
      attrs.access_ = Cxx::Private;
      break;
   default:
      return Report("Internal error: unexpected warning type.");
   }

   return ChangeAccess(log.item_, attrs);
}

//------------------------------------------------------------------------------

word Editor::ChangeStructToClass(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeStructToClass");

   //  Start by changing the structs's forward declarations.
   //
   ChangeForwards(log.item_, Cxx::StructType, Cxx::ClassType);

   //  Look for the struct's name and then back up to "struct".
   //
   auto pos = Rfind(log.item_->GetPos(), STRUCT_STR);
   if(pos == string::npos) return NotFound(STRUCT_STR, true);
   Replace(pos, strlen(STRUCT_STR), CLASS_STR);

   //  Unless the struct began with a "public:" access control, insert one.
   //
   auto left = Find(pos, "{");
   if(left == string::npos) return NotFound("Left brace");
   auto access = FindWord(left + 1, PUBLIC_STR);
   if(access == string::npos)
   {
      string control(PUBLIC_STR);
      control.push_back(':');
      InsertLine(NextBegin(left), control);
   }

   ((Class*) log.item_)->SetClassTag(Cxx::ClassType);
   return Changed(pos);
}

//------------------------------------------------------------------------------

fn_name Editor_CodeBegin = "Editor.CodeBegin";

size_t Editor::CodeBegin() const
{
   Debug::ft(Editor_CodeBegin);

   std::vector< size_t > positions;

   auto c = file_->Classes();
   if(!c->empty()) positions.push_back(c->front()->GetPos());

   auto d = file_->Datas();
   if(!d->empty()) positions.push_back(d->front()->GetPos());

   auto e = file_->Enums();
   if(!e->empty()) positions.push_back(e->front()->GetPos());

   auto f = file_->Funcs();
   if(!f->empty()) positions.push_back(f->front()->GetPos());

   auto t = file_->Types();
   if(!t->empty()) positions.push_back(t->front()->GetPos());

   size_t pos = string::npos;

   for(auto p = positions.cbegin(); p != positions.end(); ++p)
   {
      if(*p < pos) pos = *p;
   }

   auto ns = false;

   for(pos = PrevBegin(pos); pos != 0; pos = PrevBegin(pos))
   {
      auto type = PosToType(pos);

      if(!LineTypeAttr::Attrs[type].isCode && (type != FileComment))
      {
         //  Keep moving up the file.  The idea is to stop at an
         //  #include, forward declaration, or using statement.
         //
         continue;
      }

      switch(type)
      {
      case OpenBrace:
         //
         //  This should be the brace for a namespace enclosure because
         //  we started going up the file from the first code item that
         //  could be followed by one (class, enum, or function).
         //
         ns = true;
         break;

      case CodeLine:
         //
         //  If we saw an open brace, this should be a namespace enclosure.
         //  If it is, continue to back up.  If a namespace is expected but
         //  not found, generate a log.  In this case, or when a namespace
         //  *wasn't* expected, assume that the code starts after the line
         //  that we're now on, which is probably a forward declaration.
         //
         if(ns)
         {
            if(LineFind(pos, NAMESPACE_STR) != string::npos) continue;
            Debug::SwLog(Editor_CodeBegin, "namespace expected", pos);
         }
         return NextBegin(pos);

      case AccessControl:
      case DebugFt:
      case FunctionName:
         //
         //  These shouldn't occur.
         //
         Debug::SwLog(Editor_CodeBegin, "unexpected line type", type);
         //  [[fallthrough]]
      case FileComment:
      case CloseBrace:
      case CloseBraceSemicolon:
      case IncludeDirective:
      case HashDirective:
      case UsingStatement:
      default:
         //
         //  We're now one line above what should be the start of the
         //  file's code, plus any relevant comments that precede it.
         //
         return NextBegin(pos);
      }
   }

   return pos;
}

//------------------------------------------------------------------------------

bool Editor::CodeFollowsImmediately(size_t pos) const
{
   Debug::ft("Editor.CodeFollowsImmediately");

   //  Proceed from POS, skipping blank lines and access controls.  Return
   //  false if the next thing is executable code (this excludes braces and
   //  access controls), else return false.
   //
   pos = NextBegin(pos);
   if(pos == string::npos) return false;
   return (PosToType(pos) == CodeLine);
}

//------------------------------------------------------------------------------

bool Editor::CommentFollows(size_t pos) const
{
   Debug::ft("Editor.CommentFollows");

   pos = LineFindNext(pos);
   if(pos == string::npos) return false;
   if(CompareCode(pos, COMMENT_STR) == 0) return true;
   return (CompareCode(pos, COMMENT_BEGIN_STR) == 0);
}

//------------------------------------------------------------------------------

void Editor::Commit(CliThread& cli)
{
   Debug::ft("Editor.Commit");

   while(!Editors_.empty())
   {
      string expl;
      auto editor = Editors_.cbegin();

      while(true)
      {
         auto rc = (*editor)->Format(expl);
         *cli.obuf << spaces(2) << expl;
         expl.clear();

         if(rc == EditCompleted)
         {
            ++Commits_;
            break;
         }

         if(!cli.BoolPrompt(CommitFailedPrompt)) break;
      }

      Editors_.erase(editor);
   }
}

//------------------------------------------------------------------------------

size_t Editor::CommitCount() { return Commits_; }

//------------------------------------------------------------------------------

word Editor::ConvertTabsToBlanks()
{
   Debug::ft("Editor.ConvertTabsToBlanks");

   auto indent = IndentSize();

   //  Run through the source, looking for tabs.
   //
   for(size_t pos = source_.find(TAB); pos != string::npos;
      pos = source_.find(TAB, pos))
   {
      //  Find the start of this line.  If the tab appears in a comment,
      //  ignore it.  Otherwise determine how many spaces to insert when
      //  replacing the tab.
      //
      auto begin = CurrBegin(pos);
      auto end = LineFind(begin, COMMENT_STR);
      if(pos >= end) continue;

      auto count = (pos - begin) % indent;
      if(count == 0) count = indent;
      Erase(pos, 1);
      Insert(pos, spaces(count));
      Changed();
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

size_t Editor::CutCode(const CxxToken* item, string& code)
{
   Debug::ft("Editor.CutCode");

   code.clear();

   if(item == nullptr)
   {
      Report("Internal error: no item specified.");
      return string::npos;
   }

   size_t begin, end;
   GetSpan(item, begin, end);

   //  Extract the code bounded by [begin, end].  If BEGIN and END are not
   //  on the same line, then
   //  o if BEGIN is not at the start of its line, insert a CRLF before it
   //  o if END is not at the end of its line, insert a CRLF after it (and
   //    include it in the code) and re-indent that line
   //
   if(!OnSameLine(begin, end))
   {
      if(source_[begin - 1] != CRLF)
      {
         Insert(begin, CRLF_STR);
         ++begin;
         ++end;
      }

      if(source_[end] != CRLF)
      {
         auto indent = CRLF_STR + spaces(end - CurrBegin(end));
         Insert(end + 1, indent);
         ++end;
      }
   }

   code = source_.substr(begin, end - begin + 1);
   Erase(begin, end - begin + 1);
   return begin;
}

//------------------------------------------------------------------------------

string Editor::DebugFtCode(const string& fname) const
{
   Debug::ft("Editor.DebugFtCode");

   auto call = string(IndentSize(), SPACE) + "Debug::ft(";
   call.append(fname);
   call.append(");");
   return call;
}

//------------------------------------------------------------------------------

word Editor::DeleteSpecialFunction(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.DeleteSpecialFunction");

   //  This is logged on a class when the function doesn't yet exist.
   //
   if(log.item_->Type() == Cxx::Class)
   {
      return InsertSpecialFunctions(cli, log.item_);
   }

   switch(log.warning_)
   {
   case CopyCtorNotDeleted:
   case CopyOperNotDeleted:
   case CtorCouldBeDeleted:
      break;
   default:
      return Report("Unsupported warning");
   }

   //  If the function has a definition, erase it.
   //
   auto decl = static_cast< const Function* >(log.item_);
   auto defn = decl->GetDefn();
   if(defn != decl)
   {
      if(EraseItem(defn) == EditFailed) return EditFailed;
   }

   //  Find the function declaration and insert " = delete" before its
   //  closing semicolon.
   //
   size_t begin, end;
   if(!decl->GetSpan2(begin, end)) return NotFound("Function declaration");
   auto pos = CurrBegin(end);
   auto semi = source_.find(';', pos);
   if(semi == string::npos) return NotFound("Function semicolon");
   Insert(semi, " = delete");
   const_cast< Function* >(decl)->SetDeleted();

   //  Ensure that the function's access control is public.
   //
   ItemDeclAttrs attrs(decl);
   if(attrs.access_ != Cxx::Public)
   {
      attrs.access_ = Cxx::Public;
      auto item = const_cast< CxxToken* >(log.item_);
      return ChangeAccess(item, attrs);
   }

   return Changed(semi);
}

//------------------------------------------------------------------------------

void Editor::Display(ostream& stream,
   const string& prefix, const Flags& options) const
{
   Lexer::Display(stream, prefix, options);

   stream << prefix << "file     : " <<
      (file_ != nullptr ? file_->Name() : "no file specified") << CRLF;
   stream << prefix << "sorted   : " << sorted_ << CRLF;
   stream << prefix << "aliased  : " << aliased_ << CRLF;
   stream << prefix << "warnings : " << warnings_.size() << CRLF;
}

//------------------------------------------------------------------------------

bool Editor::DisplayLog
   (const CliThread& cli, const CodeWarning& log, bool file) const
{
   Debug::ft("Editor.DisplayLog");

   if(file)
   {
      *cli.obuf << log.File()->Name() << ':' << CRLF;
   }

   //  Display LOG's details.
   //
   *cli.obuf << "  Line " << log.Line() + 1;
   if(log.offset_ > 0) *cli.obuf << '/' << log.offset_;
   *cli.obuf << ": " << Warning(log.warning_);
   if(log.HasInfoToDisplay()) *cli.obuf << ": " << log.info_;
   *cli.obuf << CRLF;

   if(log.HasCodeToDisplay())
   {
      //  Display the current version of the code associated with LOG.
      //
      *cli.obuf << spaces(2);
      auto code = GetCode(log.Pos());

      if(code.empty())
      {
         *cli.obuf << "Code not found." << CRLF;
         return false;
      }

      if(code.find_first_not_of(WhitespaceChars) == string::npos)
      {
         *cli.obuf << "[line contains only whitespace]" << CRLF;
         return true;
      }

      *cli.obuf << code;
   }

   return true;
}

//------------------------------------------------------------------------------

size_t Editor::Erase(size_t pos, size_t count)
{
   Debug::ft("Editor.Erase");

   if(count == 0) return pos;
   source_.erase(pos, count);
   lastCut_ = pos;
   UpdatePos(Erased, pos, count);
   return pos;
}

//------------------------------------------------------------------------------

word Editor::EraseAccessControl(const CodeWarning& log)
{
   Debug::ft("Editor.EraseAccessControl");

   //  The parser logs RedundantAccessControl at the position
   //  where it occurred; log.item_ is nullptr in this warning.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Access control position");
   size_t len = 0;

   //  Look for the access control keyword and note its length.
   //
   auto access = LineFind(begin, PUBLIC_STR);

   while(true)
   {
      if(access != string::npos)
      {
         len = strlen(PUBLIC_STR);
         break;
      }

      access = LineFind(begin, PROTECTED_STR);

      if(access != string::npos)
      {
         len = strlen(PROTECTED_STR);
         break;
      }

      access = LineFind(begin, PRIVATE_STR);

      if(access != string::npos)
      {
         len = strlen(PRIVATE_STR);
         break;
      }

      return NotFound("Access control keyword");
   }

   //  Look for the colon that follows the keyword.
   //
   auto colon = FindNonBlank(access + len);
   if((colon == string::npos) || (source_[colon] != ':'))
      return NotFound("Colon after access control");

   //  Erase the keyword and colon.
   //
   Erase(colon, 1);
   Erase(access, len);
   return Changed(access);
}

//------------------------------------------------------------------------------

word Editor::EraseAdjacentSpaces(const CodeWarning& log)
{
   Debug::ft("Editor.EraseAdjacentSpaces");

   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Line with adjacent spaces");
   auto pos = LineFindFirst(begin);
   if(pos == string::npos) return NotFound("Adjacent spaces");

   //  If this line has a trailing comment that is aligned with one on the
   //  previous or the next line, keep the comments aligned by moving the
   //  erased spaces immediately to the left of the comment.
   //
   auto move = false;
   auto cpos = FindComment(pos);

   if(cpos != string::npos)
   {
      cpos -= begin;

      if(pos != begin)
      {
         auto prev = PrevBegin(begin);
         if(prev != string::npos)
         {
            move = (cpos == (FindComment(prev) - prev));
         }
      }

      if(!move)
      {
         auto next = NextBegin(begin);
         if(next != string::npos)
         {
            move = (cpos == (FindComment(next) - next));
         }
      }
   }

   //  Don't erase adjacent spaces that precede a trailing comment.
   //
   auto stop = cpos;

   if(stop != string::npos)
      while(IsBlank(source_[stop - 1])) --stop;
   else
      stop = CurrEnd(begin);

   cpos = stop;  // (comm - stop) will be number of erased spaces

   while(pos + 1 < stop)
   {
      if(IsBlank(source_[pos]) && IsBlank(source_[pos + 1]))
      {
         Erase(pos, 1);
         --stop;
      }
      else
      {
         ++pos;
      }
   }

   if(move) Insert(stop, string(cpos - stop, SPACE));
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::EraseArgument(const Function* func, word offset)
{
   Debug::ft("Editor.EraseArgument");

   //  In this function invocation, erase the argument at OFFSET.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

fn_name Editor_EraseAssignment = "Editor.EraseAssignment";

word Editor::EraseAssignment(const CxxToken* item)
{
   Debug::ft(Editor_EraseAssignment);

   //  ITEM should be a TypeName or QualName at the beginning of an
   //  assignment statement or a constructor's member initialization.
   //
   switch(item->Type())
   {
   case Cxx::QualName:
   case Cxx::TypeName:
   case Cxx::MemberInit:
      break;
   default:
      Debug::SwLog(Editor_EraseAssignment, strClass(item, false), 0);
      return EditFailed;
   }

   auto statement = item->GetFile()->PosToItem(item->GetPos());
   string code;

   if(CutCode(item, code) == string::npos) return EditFailed;
   if(statement != nullptr)
      statement->Delete();
   else
      const_cast< CxxToken* >(item)->Delete();
   return Changed();
}

//------------------------------------------------------------------------------

word Editor::EraseBlankLine(const CodeWarning& log)
{
   Debug::ft("Editor.EraseBlankLine");

   //  Remove the specified line of code.
   //
   EraseLine(log.Pos());
   return Changed();
}

//------------------------------------------------------------------------------

word Editor::EraseClass(const CodeWarning& log)
{
   Debug::ft("Editor.EraseClass");

   //  Erase the class's definition and the definitions of its functions
   //  and static data.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::EraseConst(const CodeWarning& log)
{
   Debug::ft("Editor.EraseConst");

   //  There are two places for a redundant "const" after the typename:
   //    "const" <typename> "const" [<typetags>] "const"
   //  The log indicates the position of the redundant "const".  If there is
   //  more than one of them, each one causes a log, so we can erase them one
   //  at a time.  To ensure that the wrong "const" is not erased after other
   //  edits have occurred on this line, verify that the const is not preceded
   //  by certain punctuation.  A preceding '(' or ',' can only occur if the
   //  "const" precedes its typename, and a '*' can only occur if the "const"
   //  makes a pointer const.
   //
   for(auto pos = FindWord(log.Pos(), CONST_STR); pos != string::npos;
      pos = FindWord(pos + 1, CONST_STR))
   {
      auto prev = RfindNonBlank(pos - 1);

      //  This is the first non-blank character preceding a subsequent
      //  "const".  If it's a pointer tag, it makes the pointer const,
      //  so continue with the next "const".
      //
      switch(source_[prev])
      {
      case '*':
      case ',':
      case '(':
         continue;
      }

      //  This is the redundant const, so erase it.  Also erase a space
      //  between it and the previous non-blank character.
      //
      Erase(pos, strlen(CONST_STR));

      if(OnSameLine(prev, pos) && (pos - prev > 1))
      {
         Erase(prev + 1, 1);
      }

      return Changed(pos);
   }

   return NotFound("Redundant const");
}

//------------------------------------------------------------------------------

word Editor::EraseDefaultValue(const Function* func, word offset)
{
   Debug::ft("Editor.EraseDefaultValue");

   //  Erase this argument's default value.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::EraseEmptyNamespace(const SpaceDefn* ns)
{
   Debug::ft("Editor.EraseEmptyNamespace");

   if(ns == nullptr) return EditSucceeded;
   size_t begin, left, end;
   if(!ns->GetSpan3(begin, left, end)) return EditSucceeded;
   auto pos = NextPos(left + 1);
   if(source_[pos] != '}') return EditSucceeded;
   return EraseItem(ns);
}

//------------------------------------------------------------------------------

word Editor::EraseExplicitTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseExplicitTag");

   auto exp = Rfind(log.item_->GetPos(), EXPLICIT_STR);
   if(exp == string::npos) return NotFound(EXPLICIT_STR, true);
   Erase(exp, strlen(EXPLICIT_STR) + 1);
   ((Function*) log.item_)->SetExplicit(false);
   return Changed(exp);
}

//------------------------------------------------------------------------------

word Editor::EraseForward(const CodeWarning& log)
{
   Debug::ft("Editor.EraseForward");

   //  Erasing the forward declaration may leave an empty enclosing
   //  namespace that should be deleted.
   //
   auto ns = file_->FindNamespaceDefn(log.item_);
   if(EraseItem(log.item_) == EditFailed) return EditFailed;
   return EraseEmptyNamespace(ns);
}

//------------------------------------------------------------------------------

word Editor::EraseItem(const CxxToken* item)
{
   Debug::ft("Editor.EraseItem");

   string code;
   if(CutCode(item, code) == string::npos) return EditFailed;
   const_cast< CxxToken* >(item)->Delete();
   return Changed();
}

//------------------------------------------------------------------------------

size_t Editor::EraseLine(size_t pos)
{
   Debug::ft("Editor.EraseLine");

   auto begin = CurrBegin(pos);
   auto end = CurrEnd(pos);
   Erase(begin, end - begin + 1);
   return begin;
}

//------------------------------------------------------------------------------

bool Editor::EraseLineBreak(size_t pos)
{
   Debug::ft("Editor.EraseLineBreak(pos)");

   auto space = CheckLineMerge(GetLineNum(pos));
   if(space < 0) return false;

   //  Merge the lines after replacing or erasing CURR's endline.
   //
   auto next = NextBegin(pos);
   auto start = LineFindFirst(next);

   if(space != 0)
   {
      Replace(next - 1, 1, SPACE_STR);
      Erase(next, start - next);
   }
   else
   {
      Erase(next - 1, start - next + 1);
   }

   return true;
}

//------------------------------------------------------------------------------

word Editor::EraseLineBreak(const CodeWarning& log)
{
   Debug::ft("Editor.EraseLineBreak(log)");

   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position for line break");
   auto merged = EraseLineBreak(begin);
   if(!merged) return Report("Line break was not removed.");
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::EraseMutableTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseMutableTag");

   //  Find the line on which the data's type appears, and erase the
   //  "mutable" before that type.
   //
   auto tag = Rfind(log.item_->GetTypeSpec()->GetPos(), MUTABLE_STR);
   if(tag == string::npos) return NotFound(MUTABLE_STR, true);
   Erase(tag, strlen(MUTABLE_STR) + 1);
   ((ClassData*) log.item_)->SetMutable(false);
   return Changed(tag);
}

//------------------------------------------------------------------------------

word Editor::EraseNoexceptTag(const Function* func)
{
   Debug::ft("Editor.EraseNoexceptTag");

   //  Look for "noexcept" just before the end of the function's signature.
   //
   auto endsig = FindSigEnd(func);
   if(endsig == string::npos) return NotFound("Signature end");
   endsig = Rfind(endsig, NOEXCEPT_STR);
   if(endsig == string::npos) return NotFound(NOEXCEPT_STR, true);
   size_t space = (IsFirstNonBlank(endsig) ? 0 : 1);
   Erase(endsig - space, strlen(NOEXCEPT_STR) + space);
   const_cast< Function* >(func)->SetNoexcept(false);
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::EraseOverrideTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseOverrideTag");

   //  Look for "override" just before the end of the function's signature.
   //
   auto endsig = FindSigEnd(log);
   if(endsig == string::npos) return NotFound("Signature end");
   endsig = Rfind(endsig, OVERRIDE_STR);
   if(endsig == string::npos) return NotFound(OVERRIDE_STR, true);
   size_t space = (IsFirstNonBlank(endsig) ? 0 : 1);
   Erase(endsig - space, strlen(OVERRIDE_STR) + space);
   ((Function*) log.item_)->SetOverride(false);
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::EraseParameter(const Function* func, word offset)
{
   Debug::ft("Editor.EraseParameter");

   //  Erase the argument at OFFSET in this function definition or declaration.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::EraseScope(const CodeWarning& log)
{
   Debug::ft("Editor.EraseScope");

   auto qname = static_cast< const QualName* >(log.item_);
   auto begin = qname->GetPos();
   auto op = source_.find(SCOPE_STR, begin);
   if(op == string::npos) return NotFound("Scope resolution operator");
   Erase(begin, op - begin + 2);
   qname->First()->Delete();
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::EraseSemicolon(const CodeWarning& log)
{
   Debug::ft("Editor.EraseSemicolon");

   //  The parser logs a redundant semicolon that follows the closing '}'
   //  of a function definition or namespace enclosure.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position of semicolon");
   auto semi = FindFirstOf(";", begin);
   if(semi == string::npos) return NotFound("Semicolon");
   auto brace = RfindNonBlank(semi - 1);
   if(brace == string::npos) return NotFound("Right brace");
   if(source_[brace] != '}') return NotFound("Right brace");
   Erase(semi, 1);
   return Changed(semi);
}

//------------------------------------------------------------------------------

word Editor::EraseTrailingBlanks()
{
   Debug::ft("Editor.EraseTrailingBlanks");

   for(size_t begin = 0; begin != string::npos; begin = NextBegin(begin))
   {
      auto end = CurrEnd(begin);
      if(begin == end) continue;

      auto pos = end - 1;
      while(IsBlank(source_[pos]) && (pos >= begin)) --pos;

      if(pos < end - 1)
      {
         Erase(pos + 1, end - pos - 1);
         Changed();
      }
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::EraseVirtualTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseVirtualTag");

   //  Look for "virtual" just before the function's return type.
   //
   auto virt = LineRfind(log.item_->GetTypeSpec()->GetPos(), VIRTUAL_STR);
   if(virt == string::npos) return NotFound(VIRTUAL_STR, true);
   Erase(virt, strlen(VIRTUAL_STR) + 1);
   ((Function*) log.item_)->SetVirtual(false);
   return Changed(virt);
}

//------------------------------------------------------------------------------

word Editor::EraseVoidArgument(const CodeWarning& log)
{
   Debug::ft("Editor.EraseVoidArgument");

   //  The function might return "void", so the second occurrence of "void"
   //  could be the argument.  Erase it, leaving only the parentheses.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position of void argument");

   for(auto arg = FindWord(begin, VOID_STR); arg != string::npos;
      arg = FindWord(arg + 1, VOID_STR))
   {
      auto lpar = RfindNonBlank(arg - 1);
      if(lpar == string::npos) continue;
      if(source_[lpar] != '(') continue;
      auto rpar = FindNonBlank(arg + strlen(VOID_STR));
      if(rpar == string::npos) break;
      if(source_[rpar] != ')') continue;
      if(OnSameLine(arg, lpar) && OnSameLine(arg, rpar))
      {
         Erase(lpar + 1, rpar - lpar - 1);
         return Changed(lpar);
      }
      Erase(arg, strlen(VOID_STR));
      return Changed(arg);
   }

   return NotFound(VOID_STR, true);
}

//------------------------------------------------------------------------------

size_t Editor::FindArgsEnd(const Function* func) const
{
   Debug::ft("Editor.FindArgsEnd");

   auto name = func->GetPos();
   if(name == string::npos) return string::npos;
   auto lpar = FindFirstOf("(", name);
   if(lpar == string::npos) return string::npos;
   auto rpar = FindClosing('(', ')', lpar + 1);
   return rpar;
}

//------------------------------------------------------------------------------

word Editor::FindFuncDefnLoc(const CodeFile* file, const CxxArea* area,
   const string& name, ItemDefnAttrs& attrs) const
{
   Debug::ft("Editor.FindFuncDefnLoc(name)");

   //  Look at all the functions that are defined in this file and that belong
   //  to AREA.
   //
   auto defns = file->GetFuncDefnsToSort();
   const Function* prev = nullptr;
   const Function* next = nullptr;
   auto reached = false;

   for(auto f = defns.cbegin(); f != defns.cend(); ++f)
   {
      //  If the current function is in another area, insert the new function
      //  o immediately before it, if the new function's class has been reached
      //  o somewhere after it, if the new function's class has not been reached
      //
      if((*f)->GetArea() != area)
      {
         if(reached)
         {
            next = (*f)->GetDefn();
            break;
         }

         prev = (*f)->GetDefn();
         continue;
      }

      //  The current function is in the same area.  Insert the new function
      //  in the cardinal order defined by FunctionRole.  When the roles match,
      //  insert the new function alphabetically.
      //
      reached = true;
      auto currRole = (*f)->FuncRole();

      if(attrs.role_ < currRole)
      {
         next = (*f)->GetDefn();
         break;
      }
      else if(attrs.role_ > currRole)
      {
         prev = (*f)->GetDefn();
         continue;
      }

      auto currName = (*f)->Name();
      auto sort = currName.compare(name);

      if(sort > 0)
      {
         next = (*f)->GetDefn();
         break;
      }
      else if(sort < 0)
      {
         prev = (*f)->GetDefn();
         continue;
      }
      else
      {
         return Report("A definition for this function already exists.");
      }
   }

   //  We now know the functions between which the new function should be
   //  inserted (PREV and NEXT), so find its precise insertion location
   //  and attributes.
   //
   return UpdateItemDefnLoc(prev, nullptr, next, attrs);
}

//------------------------------------------------------------------------------

word Editor::FindItemDeclLoc
   (const Class* cls, const string& name, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.FindItemDeclLoc");

   auto where = attrs.CalcDeclOrder();
   auto items = cls->Items();
   const CxxToken* prev = nullptr;
   const CxxToken* next = nullptr;

   for(auto i = items.cbegin(); i != items.cend(); ++i)
   {
      ItemDeclAttrs currAttrs(*i);
      auto order = currAttrs.CalcDeclOrder();

      if(where < order)
      {
         next = *i;
         break;
      }
      else if(where == order)
      {
         if(strCompare((*i)->Name(), name) > 0)
         {
            next = *i;
            break;
         }
      }

      prev = *i;
   }

   //  We now know the items between which the new item should be inserted
   //  (PREV and NEXT), so update its insertion attributes.
   //
   return UpdateItemDeclLoc(cls, prev, next, attrs);
}

//------------------------------------------------------------------------------

CodeWarning* Editor::FindLog(Warning warning, const CxxToken* item, word offset)
{
   Debug::ft("Editor.FindLog");

   for(auto w = warnings_.begin(); w != warnings_.end(); ++w)
   {
      if(((*w)->warning_ == warning) && ((*w)->item_ == item) &&
         ((*w)->offset_ == offset))
      {
         return *w;
      }
   }

   return nullptr;
}

//------------------------------------------------------------------------------

size_t Editor::FindSigEnd(const CodeWarning& log)
{
   Debug::ft("Editor.FindSigEnd(log)");

   if(log.item_ == nullptr) return string::npos;
   if(log.item_->Type() != Cxx::Function) return string::npos;
   return FindSigEnd(static_cast< const Function* >(log.item_));
}

//------------------------------------------------------------------------------

size_t Editor::FindSigEnd(const Function* func)
{
   Debug::ft("Editor.FindSigEnd(func)");

   //  Look for the first semicolon or left brace after the function's name.
   //
   return FindFirstOf(";{", func->GetPos());
}

//------------------------------------------------------------------------------

word Editor::FindSpecialFuncDeclLoc
   (CliThread& cli, const Class* cls, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.FindSpecialFuncDeclLoc");

   auto base = cls->IsBaseClass();
   auto sbase = cls->IsSingletonBase();
   auto inst = cls->WasCreated(false);
   auto solo = cls->IsSingleton();
   auto prompt = false;
   Function* prototype = cls->FindFuncByRole(PureDtor, false);
   if(prototype == nullptr) prototype = cls->FindFuncByRole(CopyCtor, false);
   if(prototype == nullptr) prototype = cls->FindFuncByRole(CopyOper, false);

   //  Update ATTRS based on what function is being inserted.
   //
   switch(attrs.role_)
   {
   case PureCtor:
      prototype = nullptr;

      if(solo)
         attrs.access_ = Cxx::Private;
      else if(base && !inst)
         attrs.access_ = Cxx::Protected;
      else if(!inst)
         prompt = true;
      break;

   case PureDtor:
      if(base)
         attrs.virt_ = true;
      else if(solo)
         attrs.access_ = Cxx::Private;
      break;

   case CopyCtor:
   case CopyOper:
      if(sbase || solo)
      {
         attrs.deleted_ = true;
      }
      else
      {
         if(base && !inst) attrs.access_ = Cxx::Protected;
         prompt = true;
      }
      break;
   };

   //  The user can decide to define the function as defaulted or deleted.
   //  If it will be defaulted and a related function is not trivial, ask
   //  if the user wants to create a shell for implmenting the function.
   //
   if(prompt)
   {
      std::ostringstream stream;
      stream << spaces(2) << "Define the " << attrs.role_ << ": " << DefnPrompt;
      auto response = cli.IntPrompt(stream.str(), 0, 2);
      if(response == 0) return Report(FixSkipped);
      attrs.deleted_ = (response == 2);
   }

   if(attrs.deleted_)
   {
      attrs.access_ = Cxx::Public;
   }
   else
   {
      if((prototype != nullptr) && !prototype->IsTrivial())
      {
         std::ostringstream stream;
         stream << "The " << prototype->FuncRole();
         stream << " is not trivial." << CRLF;
         stream << spaces(2) << ShellPrompt;
         attrs.shell_ = cli.BoolPrompt(stream.str());
      }
   }

   return FindItemDeclLoc(cls, EMPTY_STR, attrs);
}

//------------------------------------------------------------------------------

CxxNamedSet Editor::FindUsingReferents(CxxToken* item) const
{
   Debug::ft("Editor.FindUsingReferents");

   CxxUsageSets symbols;
   CxxNamedSet refs;

   item->GetUsages(*file_, symbols);

   for(auto u = symbols.users.cbegin(); u != symbols.users.cend(); ++u)
   {
      refs.insert((*u)->DirectType());
   }

   return refs;
}

//------------------------------------------------------------------------------

word Editor::Fix(CliThread& cli, const FixOptions& opts, string& expl)
{
   Debug::ft("Editor.Fix");

   //  Run through all the warnings.
   //
   word rc = 0;
   auto reply = 'y';
   auto found = false;
   auto fixed = false;
   auto first = true;
   auto exit = false;

   for(auto item = warnings_.begin(); item != warnings_.end(); ++item)
   {
      //  Skip this item if the user didn't include its warning type.
      //
      if((opts.warning != AllWarnings) &&
         (opts.warning != (*item)->warning_)) continue;

      switch(FixStatus(**item))
      {
      case NotFixed:
         //
         //  This item is eligible for fixing, and note that such an item
         //  was found.
         //
         found = true;
         break;

      case Fixed:
      case Pending:
         //
         //  Before skipping over an eligible item that was already fixed,
         //  note that such an item was found.
         //
         fixed = true;
         continue;

      case Nullified:
         if(opts.warning == AllWarnings) continue;
         return Report(ItemDeleted, EditAbort);

      case NotSupported:
      default:
         //
         //  If multiple warning types are being fixed, try the next one.  If
         //  only one warning type was selected, there will be nothing to fix,
         //  so return a value that will terminate the >fix command.
         //
         if(opts.warning == AllWarnings) continue;
         return Report(NotImplemented, EditAbort);
      }

      //  This item is eligible for fixing.  Display it.
      //
      if(DisplayLog(cli, **item, first))
      {
         first = false;

         //  See if the user wishes to fix the item.  Valid responses are
         //  o 'y' = fix it, which invokes the appropriate function
         //  o 'n' = don't fix it
         //  o 's' = skip this file
         //  o 'q' = done fixing warnings
         //
         expl.clear();
         reply = 'y';

         if(opts.prompt)
         {
            std::ostringstream stream;
            stream << spaces(2) << FixPrompt;
            reply = cli.CharPrompt(stream.str(), YNSQChars, YNSQHelp);
         }

         switch(reply)
         {
         case 'y':
         {
            auto logs = (*item)->LogsToFix(expl);

            if(!expl.empty())
            {
               *cli.obuf << spaces(2) << expl << CRLF;
               expl.clear();
            }

            for(auto log = logs.begin(); log != logs.end(); ++log)
            {
               auto& editor = (*log)->File()->GetEditor();
               rc = editor.FixLog(cli, **log);
               ReportFix(cli, *log, rc);
            }

            break;
         }

         case 'n':
            break;

         case 's':
         case 'q':
            exit = true;
            break;

         default:
            return Report("Internal error: unknown response.", -6);
         }
      }

      cli.Flush();
      if(!opts.prompt) ThisThread::Pause(Duration(20, mSECS));
      if(exit || (rc < EditFailed)) break;
   }

   if(found)
   {
      if(exit || (rc < EditFailed))
         *cli.obuf << "Remaining warnings skipped." << CRLF;
      else
         *cli.obuf << "End of warnings." << CRLF;
   }
   else if(fixed)
   {
      *cli.obuf << "Selected warning(s) in " << file_->Name();
      *cli.obuf << " previously fixed." << CRLF;
   }
   else
   {
      if(!opts.multiple)
      {
         *cli.obuf << "No warnings that can be fixed were found." << CRLF;
      }
   }

   //  Returning -1 or greater indicates that the next file can still be
   //  processed, so return a lower value if the user wants to quit or if
   //  a serious error occurred.  In that case, clear EXPL, since it has
   //  already been displayed when writing the last file.
   //
   Commit(cli);

   if((reply == 'q') && (rc >= EditFailed))
   {
      expl.clear();
      rc = -2;
   }

   return rc;
}

//------------------------------------------------------------------------------

word Editor::FixData(const Data* data, const CodeWarning& log)
{
   Debug::ft("Editor.FixData");

   switch(log.warning_)
   {
   case DataUnused:
      return EraseItem(data);
   case DataInitOnly:
      return EraseItem(data);
   case DataWriteOnly:
      return EraseItem(data);
   case DataCouldBeConst:
      return TagAsConstData(data);
   case DataCouldBeConstPtr:
      return TagAsConstPointer(data);
   case DataCouldBeFree:
      return ChangeDataToFree(nullptr, data);
   }

   return Report("Internal error: unsupported data warning.");
}

//------------------------------------------------------------------------------

word Editor::FixDatas(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixDatas");

   if(log.item_->Type() != Cxx::Data)
   {
      return Report("Internal error: warning is not for data.");
   }

   //  Fix the data's declaration and definition (if distinct).
   //
   auto data = static_cast< const Data* >(log.item_);
   std::vector< const Data* > datas;
   GetDatas(data, datas);

   for(auto d = datas.cbegin(); d != datas.cend(); ++d)
   {
      auto& editor = (*d)->GetFile()->GetEditor();
      auto rc = editor.FixData(*d, log);
      editor.ReportFixInFile(cli, &log, rc);
   }

   return EditCompleted;
}

//------------------------------------------------------------------------------

word Editor::FixFunction(const Function* func, const CodeWarning& log)
{
   Debug::ft("Editor.FixFunction");

   switch(log.warning_)
   {
   case ArgumentUnused:
      return EraseParameter(func, log.offset_);
   case FunctionUnused:
      return EraseItem(func);
   case VirtualAndPublic:
      return SplitVirtualFunction(func);
   case VirtualDefaultArgument:
      return EraseDefaultValue(func, log.offset_);
   case ArgumentCouldBeConstRef:
      return TagAsConstReference(func, log.offset_);
   case ArgumentCouldBeConst:
      return TagAsConstArgument(func, log.offset_);
   case FunctionCouldBeConst:
      return TagAsConstFunction(func);
   case FunctionCouldBeStatic:
      return TagAsStaticFunction(func);
   case FunctionCouldBeFree:
      return ChangeFunctionToFree(func);
   case FunctionCouldBeDefaulted:
      return TagAsDefaulted(func);
   case CouldBeNoexcept:
      return TagAsNoexcept(func);
   case ShouldNotBeNoexcept:
      return EraseNoexceptTag(func);
   case FunctionCouldBeMember:
      return ChangeFunctionToMember(func, log.offset_);
   }

   return Report("Internal error: unsupported function warning.");
}

//------------------------------------------------------------------------------

word Editor::FixFunctions(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixFunctions");

   if(log.item_->Type() != Cxx::Function)
   {
      return Report("Internal error: warning is not for a function.");
   }

   //  Create a list of all the function declarations and definitions that
   //  are associated with the log.
   //
   auto func = static_cast< const Function* >(log.item_);
   std::vector< const Function* > funcs;
   GetOverrides(func, funcs);

   for(auto f = funcs.cbegin(); f != funcs.cend(); ++f)
   {
      auto& editor = (*f)->GetFile()->GetEditor();
      auto rc = editor.FixFunction(*f, log);
      editor.ReportFixInFile(cli, &log, rc);
   }

   return EditCompleted;
}

//------------------------------------------------------------------------------

word Editor::FixInvoker(const Function* func, const CodeWarning& log)
{
   Debug::ft("Editor.FixInvoker");

   switch(log.warning_)
   {
   case ArgumentUnused:
      return EraseArgument(func, log.offset_);
   case VirtualDefaultArgument:
      return InsertArgument(func, log.offset_);
   case FunctionCouldBeFree:
      return ChangeInvokerToFree(func);
   case FunctionCouldBeMember:
      return ChangeInvokerToMember(func, log.offset_);
   }

   return Report("Internal error: unexpected invoker warning.");
}

//------------------------------------------------------------------------------

word Editor::FixInvokers(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.FixInvokers");

   //  Use FixFunctions to modify all of the function signatures, and then
   //  use the cross-reference to find and modify all of the invocations.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::FixLog(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixLog");

   switch(log.status_)
   {
   case NotSupported:
      return Report(NotImplemented);
   case Fixed:
   case Pending:
      return Report("This warning has already been fixed.");
   }

   return FixWarning(cli, log);
}

//------------------------------------------------------------------------------

word Editor::FixReference(const CxxNamed* item, const CodeWarning& log)
{
   Debug::ft("Editor.FixReference");

   switch(log.warning_)
   {
   case DataWriteOnly:
      return EraseAssignment(item);
   case DataCouldBeFree:
      return ChangeDataToFree(item, static_cast< const Data* >(log.item_));
   }

   return Report("Internal error: unsupported data warning.");
}

//------------------------------------------------------------------------------

word Editor::FixReferences(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixReferences");

   if(log.item_->Type() != Cxx::Data)
   {
      return Report("Internal error: warning is not for dadta.");
   }

   //  Fix references to the data, followed by the data itself.
   //
   auto data = static_cast< const Data* >(log.item_);
   std::vector< const CxxNamed* > refs;
   GetReferences(data, refs);

   for(auto r = refs.cbegin(); r != refs.cend(); ++r)
   {
      auto& editor = (*r)->GetFile()->GetEditor();
      auto rc = editor.FixReference(*r, log);
      editor.ReportFixInFile(cli, &log, rc);
   }

   return FixDatas(cli, log);
}

//------------------------------------------------------------------------------

WarningStatus Editor::FixStatus(const CodeWarning& log) const
{
   Debug::ft("Editor.FixStatus");

   //  Some logs may appear multiple times, but all of them get fixed when
   //  the first one gets fixed.
   //
   switch(log.warning_)
   {
   case IncludeNotSorted:
   case IncludeFollowsCode:
      if((log.status_ == NotFixed) && sorted_) return Fixed;
      break;

   case FunctionNotSorted:
      if((log.status_ == NotFixed) && FunctionsWereSorted(log)) return Fixed;
      break;

   case OverrideNotSorted:
      if((log.status_ == NotFixed) && OverridesWereSorted(log)) return Fixed;
      break;
   }

   return log.status_;
}

//------------------------------------------------------------------------------

word Editor::FixWarning(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixWarning");

   switch(log.warning_)
   {
   case UseOfNull:
      return ReplaceNull(log);
   case PtrTagDetached:
      return AdjustTags(log);
   case RefTagDetached:
      return AdjustTags(log);
   case RedundantSemicolon:
      return EraseSemicolon(log);
   case RedundantConst:
      return EraseConst(log);
   case DefineNotAtFileScope:
      return MoveDefine(log);
   case IncludeFollowsCode:
      return SortIncludes();
   case IncludeGuardMissing:
      return InsertIncludeGuard(log);
   case IncludeNotSorted:
      return SortIncludes();
   case IncludeDuplicated:
      return EraseItem(log.item_);
   case IncludeAdd:
      return InsertInclude(log);
   case IncludeRemove:
      return EraseItem(log.item_);
   case RemoveOverrideTag:
      return EraseOverrideTag(log);
   case UsingInHeader:
      return ReplaceUsing(log);
   case UsingDuplicated:
      return EraseItem(log.item_);
   case UsingAdd:
      return InsertUsing(log);
   case UsingRemove:
      return EraseItem(log.item_);
   case ForwardAdd:
      return InsertForward(log);
   case ForwardRemove:
      return EraseForward(log);
   case ArgumentUnused:
      return FixInvokers(cli, log);
   case ClassUnused:
      return EraseClass(log);
   case DataUnused:
      return FixReferences(cli, log);
   case EnumUnused:
      return EraseItem(log.item_);
   case EnumeratorUnused:
      return EraseItem(log.item_);
   case FriendUnused:
      return EraseItem(log.item_);
   case FunctionUnused:
      return FixFunctions(cli, log);
   case TypedefUnused:
      return EraseItem(log.item_);
   case ForwardUnresolved:
      return EraseForward(log);
   case FriendUnresolved:
      return EraseItem(log.item_);
   case FriendAsForward:
      return InsertForward(log);
   case HidesInheritedName:
      return ReplaceName(log);
   case ClassCouldBeNamespace:
      return ChangeClassToNamespace(log);
   case ClassCouldBeStruct:
      return ChangeClassToStruct(log);
   case StructCouldBeClass:
      return ChangeStructToClass(log);
   case RedundantAccessControl:
      return EraseAccessControl(log);
   case ItemCouldBePrivate:
      return ChangeAccess(log, Cxx::Private);
   case ItemCouldBeProtected:
      return ChangeAccess(log, Cxx::Protected);
   case AnonymousEnum:
      return InsertEnumName(log);
   case DataUninitialized:
      return InsertDataInit(log);
   case DataInitOnly:
      return FixReferences(cli, log);
   case DataWriteOnly:
      return FixReferences(cli, log);
   case DataCouldBeConst:
      return FixDatas(cli, log);
   case DataCouldBeConstPtr:
      return FixDatas(cli, log);
   case DataNeedNotBeMutable:
      return EraseMutableTag(log);
   case ImplicitPODConstructor:
      return InsertPODCtor(log);
   case ImplicitConstructor:
      return InsertSpecialFunctions(cli, log.item_);
   case ImplicitCopyConstructor:
      return InsertSpecialFunctions(cli, log.item_);
   case ImplicitCopyOperator:
      return InsertSpecialFunctions(cli, log.item_);
   case PublicConstructor:
      return ChangeSpecialFunction(cli, log);
   case NonExplicitConstructor:
      return TagAsExplicit(log);
   case MemberInitMissing:
      return InsertMemberInit(log);
   case MemberInitNotSorted:
      return MoveMemberInit(log);
   case ImplicitDestructor:
      return InsertSpecialFunctions(cli, log.item_);
   case VirtualDestructor:
      return ChangeAccess(log, Cxx::Public);
   case NonVirtualDestructor:
      return ChangeSpecialFunction(cli, log);
   case RuleOf3DtorNoCopyCtor:
      return InsertSpecialFunctions(cli, log.item_);
   case RuleOf3DtorNoCopyOper:
      return InsertSpecialFunctions(cli, log.item_);
   case RuleOf3CopyCtorNoOper:
      return InsertSpecialFunctions(cli, log.item_);
   case RuleOf3CopyOperNoCtor:
      return InsertSpecialFunctions(cli, log.item_);
   case FunctionNotDefined:
      return EraseItem(log.item_);
   case PureVirtualNotDefined:
      return InsertPureVirtual(log);
   case VirtualAndPublic:
      return FixFunctions(cli, log);
   case FunctionNotOverridden:
      return EraseVirtualTag(log);
   case RemoveVirtualTag:
      return EraseVirtualTag(log);
   case OverrideTagMissing:
      return TagAsOverride(log);
   case VoidAsArgument:
      return EraseVoidArgument(log);
   case AnonymousArgument:
      return RenameArgument(cli, log);
   case DefinitionRenamesArgument:
      return RenameArgument(cli, log);
   case OverrideRenamesArgument:
      return RenameArgument(cli, log);
   case VirtualDefaultArgument:
      return FixInvokers(cli, log);
   case ArgumentCouldBeConstRef:
      return FixFunctions(cli, log);
   case ArgumentCouldBeConst:
      return FixFunctions(cli, log);
   case FunctionCouldBeConst:
      return FixFunctions(cli, log);
   case FunctionCouldBeStatic:
      return FixFunctions(cli, log);
   case FunctionCouldBeFree:
      return FixInvokers(cli, log);
   case StaticFunctionViaMember:
      return ChangeOperator(log);
   case UseOfTab:
      return ConvertTabsToBlanks();
   case Indentation:
      return AdjustIndentation(log);
   case TrailingSpace:
      return EraseTrailingBlanks();
   case AdjacentSpaces:
      return EraseAdjacentSpaces(log);
   case AddBlankLine:
      return InsertBlankLine(log);
   case RemoveLine:
      return EraseBlankLine(log);
   case LineLength:
      return InsertLineBreak(log);
   case FunctionNotSorted:
      return SortFunctions(log);
   case HeadingNotStandard:
      return ReplaceHeading(log);
   case IncludeGuardMisnamed:
      return RenameIncludeGuard(log);
   case DebugFtNotInvoked:
      return InsertDebugFtCall(cli, log);
   case DebugFtNameMismatch:
      return RenameDebugFtArgument(cli, log);
   case DebugFtNameDuplicated:
      return RenameDebugFtArgument(cli, log);
   case DisplayNotOverridden:
      return InsertDisplay(cli, log);
   case PatchNotOverridden:
      return InsertPatch(cli, log);
   case FunctionCouldBeDefaulted:
      return FixFunctions(cli, log);
   case InitCouldUseConstructor:
      return ChangeAssignmentToCtorCall(log);
   case CouldBeNoexcept:
      return FixFunctions(cli, log);
   case ShouldNotBeNoexcept:
      return FixFunctions(cli, log);
   case UseOfSlashAsterisk:
      return ReplaceSlashAsterisk(log);
   case RemoveLineBreak:
      return EraseLineBreak(log);
   case CopyCtorConstructsBase:
      return InsertCopyCtorCall(log);
   case FunctionCouldBeMember:
      return FixInvokers(cli, log);
   case ExplicitConstructor:
      return EraseExplicitTag(log);
   case BitwiseOperatorOnBoolean:
      return ChangeOperator(log);
   case DebugFtCanBeLiteral:
      return InlineDebugFtArgument(log);
   case DataCouldBeFree:
      return FixReferences(cli, log);
   case ConstructorNotPrivate:
      return ChangeSpecialFunction(cli, log);
   case DestructorNotPrivate:
      return ChangeSpecialFunction(cli, log);
   case RedundantScope:
      return EraseScope(log);
   case OperatorSpacing:
      return AdjustOperator(log);
   case PunctuationSpacing:
      return AdjustPunctuation(log);
   case CopyCtorNotDeleted:
      return DeleteSpecialFunction(cli, log);
   case CopyOperNotDeleted:
      return DeleteSpecialFunction(cli, log);
   case CtorCouldBeDeleted:
      return DeleteSpecialFunction(cli, log);
   case OverrideNotSorted:
      return SortOverrides(log);
   }

   return Report(NotImplemented);
}

//------------------------------------------------------------------------------

word Editor::Format(string& expl)
{
   Debug::ft("Editor.Format");

   EraseTrailingBlanks();
   AdjustVertically();
   ConvertTabsToBlanks();
   auto rc = Write();
   expl = GetExpl();
   return rc;
}

//------------------------------------------------------------------------------

bool Editor::FunctionsWereSorted(const CodeWarning& log) const
{
   Debug::ft("Editor.FunctionsWereSorted");

   auto area = log.item_->GetArea();

   for(auto w = warnings_.cbegin(); w != warnings_.cend(); ++w)
   {
      if((*w)->warning_ == FunctionNotSorted)
      {
         if(((*w)->status_ >= Pending) && ((*w)->item_->GetArea() == area))
            return true;
      }
   }

   return false;
}

//------------------------------------------------------------------------------

CxxNamedVector Editor::GetItemsForFuncDefn(const Function* func) const
{
   Debug::ft("Editor.GetItemsForFuncDefn");

   CxxNamedVector items;
   auto& fileItems = file_->Items();

   //  Return the items above FUNC that are referenced by FUNC.
   //
   for(auto i = fileItems.cbegin(); i != fileItems.cend(); ++i)
   {
      if(*i == func)
      {
         size_t begin, end;
         func->GetSpan2(begin, end);

         while(i != fileItems.cbegin())
         {
            --i;
            if(!ItemIsUsedBetween(*i, begin, end)) break;
            items.push_back(*i);
         }

         break;
      }
   }

   std::sort(items.begin(), items.end(), IsSortedByPos);
   return items;
}

//------------------------------------------------------------------------------

ItemOffsets Editor::GetOffsets(const CxxToken* item) const
{
   Debug::ft("Editor.GetOffsets");

   ItemOffsets offsets;

   if(item == nullptr) return offsets;

   size_t begin, end;
   if(!item->GetSpan2(begin, end)) return offsets;

   //  See what follows ITEM.
   //
   auto pos = NextBegin(end);
   auto type = PosToType(pos);

   if(type == BlankLine)
   {
      type = PosToType(NextBegin(pos));

      if(type == RuleComment)
         offsets.below_ = OffsetRule;
      else
         offsets.below_ = OffsetBlank;
   }

   //  See what precedes ITEM.
   //
   pos = begin;

   while(true)
   {
      pos = PrevBegin(pos);
      type = PosToType(pos);

      switch(type)
      {
      case BlankLine:
         offsets.above_ = OffsetBlank;
         //  [[fallthrough]]
      case FunctionName:
         continue;

      case RuleComment:
         offsets.above_ = OffsetRule;
         //  [[fallthrough]]
      default:
         return offsets;
      }
   }
}

//------------------------------------------------------------------------------

void Editor::GetSpan(const CxxToken* item, size_t& begin, size_t& end)
{
   Debug::ft("Editor.GetSpan");

   if(item == nullptr)
   {
      begin = string::npos;
      end = string::npos;
      return;
   }

   if(!item->GetSpan2(begin, end)) return;

   if(source_[begin] == ',')
   {
      //  The cut begins at a comma, which could be on the previous line.
      //  If a comment follows it, replace it with a space and cut from
      //  the beginning of the next line.
      //
      if(CommentFollows(begin + 1))
      {
         source_[begin] = SPACE;
         begin = NextBegin(begin);
      }
   }
   else
   {
      //  Cut from the start of BEGIN's line unless other code precedes
      //  the item.
      //
      if(IsFirstNonBlank(begin)) begin = CurrBegin(begin);
   }

   //  When the cuts ends at a right brace, also cut a semicolon that
   //  follows immediately.
   //
   if(source_[end] == '}')
   {
      auto pos = NextPos(end + 1);
      if((pos != string::npos) && (source_[pos] == ';')) end = pos;
   }

   //  Cut any comment or whitespace that follows on the last line.
   //
   if(NoCodeFollows(end + 1))
      end = CurrEnd(end);
   else
      end = LineFindNonBlank(end + 1) - 1;

   //  If entire lines of code that aren't immediately followed by more code
   //  have been selected, include any comment that precedes the code, as it
   //  almost certainly refers only to the code being cut.  But don't do this
   //  for a preprocessor directive or function data, whose preceding comments
   //  usually apply to code that follows, even if a blank line follows ITEM.
   //
   if((begin == CurrBegin(begin)) && (end == CurrEnd(end)))
   {
      if(source_[begin] == '#')
      {
         return;
      }

      if((item->Type() == Cxx::Data) &&
         (item->GetScope()->Type() == Cxx::Block))
      {
         return;
      }

      if(!CodeFollowsImmediately(end))
      {
         begin = IntroStart(begin, false);
      }
   }
}

//------------------------------------------------------------------------------

size_t Editor::IncludesBegin() const
{
   Debug::ft("Editor.IncludesBegin");

   for(size_t pos = 0; pos != string::npos; pos = NextBegin(pos))
   {
      if(CompareCode(pos, HASH_INCLUDE_STR) == 0) return pos;
   }

   return string::npos;
}

//------------------------------------------------------------------------------

word Editor::Indent(size_t pos)
{
   Debug::ft("Editor.Indent");

   auto info = GetLineInfo(pos);
   if(info == nullptr) return NotFound("Position for indentation");
   auto begin = CurrBegin(pos);
   auto first = LineFindFirst(pos);
   auto curr = first - begin;
   auto depth = info->depth;
   if(info->continuation) ++depth;
   auto indent = depth * IndentSize();

   if(indent > curr)
      Insert(pos, spaces(indent - curr));
   else
      Erase(pos, curr - indent);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::InlineDebugFtArgument(const CodeWarning& log)  //u
{
   Debug::ft("Editor.InlineDebugFtArgument");

   //  Find the fn_name data, in-line its string literal in the Debug::ft
   //  call, and then erase it.
   //
   string fname;
   auto data = static_cast< const Data* >(log.item_);
   if(!data->GetStrValue(fname)) return NotFound("fn_name definition");
   if(EraseItem(data) == EditFailed) return EditFailed;

   auto literal = QUOTE + fname + QUOTE;
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position of Debug::ft");
   auto lpar = source_.find('(', begin);
   if(lpar == string::npos) return NotFound("Left parenthesis");
   auto rpar = source_.find(')', lpar);
   if(rpar == string::npos) return NotFound("Right parenthesis");
   Replace(lpar + 1, rpar - lpar - 1, literal);
   return Changed(lpar);
}

//------------------------------------------------------------------------------

size_t Editor::Insert(size_t pos, const string& code)
{
   Debug::ft("Editor.Insert");

   source_.insert(pos, code);
   UpdatePos(Inserted, pos, code.size());
   return pos;
}

//------------------------------------------------------------------------------

void Editor::InsertAboveItemDecl
   (const ItemDeclAttrs& attrs, const string& comment)
{
   Debug::ft("Editor.InsertAboveItemDecl");

   if(attrs.comment_)
   {
      InsertLine(attrs.pos_, strComment(EMPTY_STR, attrs.indent_));
      Insert(attrs.pos_, strComment(comment, attrs.indent_));
   }

   if(attrs.thisctrl_)
   {
      std::ostringstream stream;
      if(attrs.indent_ > 0) stream << spaces(attrs.indent_ - IndentSize());
      stream << attrs.access_ << ':';
      InsertLine(attrs.pos_, stream.str());
   }
   else if(attrs.blank_ == BlankAbove)
   {
      InsertLine(attrs.pos_, EMPTY_STR);
   }
}

//------------------------------------------------------------------------------

void Editor::InsertAboveItemDefn(const ItemDefnAttrs& attrs)
{
   Debug::ft("Editor.InsertAboveItemDefn");

   switch(attrs.offsets_.above_)
   {
   case OffsetRule:
      InsertLine(attrs.pos_, EMPTY_STR);
      InsertRule(attrs.pos_, '-');
      //  [[fallthrough]]
   case OffsetBlank:
      InsertLine(attrs.pos_, EMPTY_STR);
   }
}

//------------------------------------------------------------------------------

word Editor::InsertArgument(const Function* func, word offset)
{
   Debug::ft("Editor.InsertArgument");

   //  Change all invocations of FUNC so that any which use the default
   //  value for this argument pass the default value explicitly.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

void Editor::InsertBelowItemDecl(const ItemDeclAttrs& attrs)
{
   Debug::ft("Editor.InsertBelowItemDecl");

   if(attrs.nextctrl_ != Cxx::Access_N)
   {
      std::ostringstream access;
      access << attrs.nextctrl_ << ':';
      InsertLine(attrs.pos_, access.str());
   }
   else if(attrs.blank_ == BlankBelow)
   {
      InsertLine(attrs.pos_, EMPTY_STR);
   }
}

//------------------------------------------------------------------------------

void Editor::InsertBelowItemDefn(const ItemDefnAttrs& attrs)
{
   Debug::ft("Editor.InsertBelowItemDefn");

   switch(attrs.offsets_.below_)
   {
   case OffsetRule:
      InsertLine(attrs.pos_, EMPTY_STR);
      InsertRule(attrs.pos_, '-');
      //  [[fallthrough]]
   case OffsetBlank:
      InsertLine(attrs.pos_, EMPTY_STR);
   }
}

//------------------------------------------------------------------------------

word Editor::InsertBlankLine(const CodeWarning& log)
{
   Debug::ft("Editor.InsertBlankLine");

   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position for blank line");
   InsertLine(begin, EMPTY_STR);
   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::InsertCopyCtorCall(const CodeWarning& log)
{
   Debug::ft("Editor.InsertCopyCtorCall");

   //  Have this copy constructor invoke its base class copy constructor.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertDataInit(const CodeWarning& log)
{
   Debug::ft("Editor.InsertDataInit");

   //  Initialize this data item to its default value.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertDebugFtCall(CliThread& cli, const CodeWarning& log)  //u
{
   Debug::ft("Editor.InsertDebugFtCall");

   size_t begin, left, right;
   auto func = static_cast< const Function* >(log.item_);
   func->GetSpan3(begin, left, right);
   if(left == string::npos) return NotFound("Function definition");

   //  Get the start of the name for an fn_name declaration and the inline
   //  string literal for the Debug::ft invocation.  Set EXTRA if anything
   //  follows the left brace on the same line.
   //
   string flit, fvar;
   DebugFtNames(func, flit, fvar);
   auto extra = (LineFindNext(left + 1) != string::npos);

   //  There are two possibilities:
   //  o An fn_name is already defined (e.g. for invoking Debug::SwLog), in
   //    which case it will be located before the end of the function.  Its
   //    name will start with FVAR but could be longer if the function is
   //    overloaded, so extract everything to the closing ')' in the call.
   //  o An fn_name is not already defined, in which case the string literal
   //    (FLIT) can be used.
   //
   string arg;

   for(auto pos = left; (pos < right) && arg.empty(); pos = NextBegin(pos))
   {
      auto start = LineFind(pos, fvar);

      if(start != string::npos)
      {
         auto end = source_.find_first_not_of(ValidNextChars, start);
         if(end == string::npos) return NotFound("End of fn_name");
         arg = source_.substr(start, end - start);
      }
   }

   if(arg.empty())
   {
      //  No fn_name was defined, so use FLIT.  If another function already
      //  uses that name, prompt the user for a suffix.
      //
      if(!EnsureUniqueDebugFtName(cli, flit, arg))
         return Report(FixSkipped);
   }

   //  Create the call to Debug::ft at the top of the function, after its
   //  left brace:
   //  o If something followed the left brace, push it down.
   //  o Insert a blank line after the call to Debug::ft.
   //  o Insert the call to Debug::ft.
   //  o If the left brace wasn't at the start of its line, push it down.
   //
   if(extra) InsertLineBreak(left + 1);
   auto below = NextBegin(left);
   InsertLine(below, EMPTY_STR);
   auto call = DebugFtCode(arg);
   InsertLine(below, call);
   if(!IsFirstNonBlank(left)) InsertLineBreak(left);
   return Changed(below);
}

//------------------------------------------------------------------------------

word Editor::InsertDisplay(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.InsertDisplay");

   //  Declare an override and put "To be implemented" in the definition, with
   //  an invocation of the base class's Display function.  A more ambitious
   //  implementation would display members or invoke *their* Display functions.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertEnumName(const CodeWarning& log)
{
   Debug::ft("Editor.InsertEnumName");

   //  Prompt for the enum's name and insert it.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertForward(const CodeWarning& log)  //u
{
   Debug::ft("Editor.InsertForward(log)");

   //  LOGS provides the forward's namespace and any template parameters.
   //
   string forward = spaces(IndentSize()) + log.info_ + ';';
   auto srPos = forward.find(SCOPE_STR);
   if(srPos == string::npos) return NotFound("Forward's namespace.");

   //  Extract the namespace.
   //
   auto areaPos = forward.find("class ");
   if(areaPos == string::npos) areaPos = forward.find("struct ");
   if(areaPos == string::npos) areaPos = forward.find("union ");
   if(areaPos == string::npos) return NotFound("Forward's area type");

   //  Set NSPACE to "namespace <ns>", where <ns> is the symbol's namespace.
   //  Erase <ns> from FORWARD and then decide where to insert the forward
   //  declaration.
   //
   string nspace = NAMESPACE_STR;
   auto nsPos = forward.find(SPACE, areaPos);
   auto nsName = forward.substr(nsPos, srPos - nsPos);
   nspace += nsName;
   forward.erase(nsPos + 1, srPos - nsPos + 1);
   auto begin = CodeBegin();

   for(auto pos = PrologEnd(); pos != string::npos; pos = NextBegin(pos))
   {
      if(CompareCode(pos, NAMESPACE_STR) == 0)
      {
         //  If this namespace matches NSPACE, add the declaration to it.
         //  If this namespace's name is alphabetically after NSPACE, add
         //  the declaration before it, along with its namespace.
         //
         auto comp = CompareCode(pos, nspace);
         if(comp == 0) return InsertForward(pos, forward);
         if(comp > 0) return InsertNamespaceForward(pos, nspace, forward);
      }
      else if((CompareCode(pos, USING_STR) == 0) || (pos == begin))
      {
         //  We have now passed any existing forward declarations, so add
         //  the new declaration here, along with its namespace.
         //
         return InsertNamespaceForward(pos, nspace, forward);
      }
   }

   return Report("Failed to insert forward declaration.");
}

//------------------------------------------------------------------------------

word Editor::InsertForward(size_t pos, const string& forward)  //u
{
   Debug::ft("Editor.InsertForward(pos)");

   //  POS references a namespace that matches the one for a new forward
   //  declaration.  Insert the new declaration alphabetically within the
   //  declarations that already appear in this namespace.  Note that it
   //  may already have been inserted while fixing another warning.
   //
   for(pos = NextBegin(pos); pos != string::npos; pos = NextBegin(pos))
   {
      auto first = LineFindFirst(pos);
      if(source_[first] == '{') continue;

      auto comp = CompareCode(pos, forward);
      if(comp == 0) return Report("Previously inserted.");

      if((comp > 0) || (source_[first] == '}'))
      {
         InsertLine(pos, forward);
         return Changed(pos);
      }
   }

   return Report("Failed to insert forward declaration.");
}

//------------------------------------------------------------------------------

word Editor::InsertInclude(const CodeWarning& log)
{
   Debug::ft("Editor.InsertInclude");

   //  The file name for the new #include directive appears in log.info_ and
   //  is enclosed in quotes or angle brackets.
   //
   auto filename = log.info_.substr(1, log.info_.size() - 2);
   IncludePtr incl(new Include(filename, log.info_.front() == '<'));
   size_t pos = string::npos;
   incl->SetLoc(file_, pos, false);
   incl->CalcGroup();

   auto& incls = file_->Includes();

   for(auto next = incls.cbegin(); next != incls.cend(); ++next)
   {
      if(IncludesAreSorted(incl, *next))
      {
         pos = (*next)->GetPos();
         break;
      }
   }

   if(pos == string::npos)
   {
      //  If we get here, the new #include must be added after all the others.
      //  The last #include should be followed by a blank line, so insert one
      //  if necessary.
      //
      if(!incls.empty())
         pos = NextBegin(incls.back()->GetPos());
      else
         pos = PrologEnd();

      if(!IsBlankLine(pos))
      {
         InsertLine(pos, EMPTY_STR);
      }
   }

   auto code = string(HASH_INCLUDE_STR) + SPACE + log.info_;
   InsertLine(pos, code);
   file_->InsertInclude(incl, pos);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::InsertIncludeGuard(const CodeWarning& log)  //u
{
   Debug::ft("Editor.InsertIncludeGuard");

   //  Insert the guard before the first #include.  If there isn't one,
   //  insert it before the first line of code.  If there isn't any code,
   //  insert it at the end of the file.
   //
   auto pos = IncludesBegin();
   if(pos == string::npos) pos = PrologEnd();
   string guardName = log.File()->MakeGuardName();
   string code = "#define " + guardName;
   pos = InsertLine(pos, EMPTY_STR);
   pos = InsertLine(pos, code);
   code = "#ifndef " + guardName;
   InsertLine(pos, code);
   code = string(HASH_ENDIF_STR) + CRLF_STR;
   Insert(source_.size(), code);
   return Changed(pos);
}

//------------------------------------------------------------------------------

size_t Editor::InsertLine(size_t pos, const string& code)
{
   Debug::ft("Editor.InsertLine");

   if(pos >= source_.size()) return string::npos;
   auto copy = code;
   if(copy.empty() || (copy.back() != CRLF)) copy.push_back(CRLF);
   return Insert(pos, copy);
}

//------------------------------------------------------------------------------

word Editor::InsertLineBreak(size_t pos)
{
   Debug::ft("Editor.InsertLineBreak(pos)");

   auto begin = CurrBegin(pos);
   if(begin == string::npos) return NotFound("Line break insertion position");
   auto end = CurrEnd(pos);
   if((pos == begin) || (pos == end)) return
      Report("Error: inserting line break at beginning or end of line.");
   if(IsBlankLine(pos)) return
      Report("Error: inserting line break in an empty line");
   Insert(pos, CRLF_STR);
   return Indent(NextBegin(pos));
}

//------------------------------------------------------------------------------

word Editor::InsertLineBreak(const CodeWarning& log)
{
   Debug::ft("Editor.InsertLineBreak(log)");

   //  Consider parentheses, lexical level, binary operators...
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position for line break");

   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertMemberInit(const CodeWarning& log)
{
   Debug::ft("Editor.InsertMemberInit");

   //  Initialize the member to its default value.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertNamespaceForward  //u
   (size_t pos, const string& nspace, const string& forward)
{
   Debug::ft("Editor.InsertNamespaceForward");

   //  Insert a new forward declaration, along with an enclosing namespace,
   //  at POS.  Offset it with blank lines.
   //
   InsertLine(pos, EMPTY_STR);
   InsertLine(pos, "}");
   InsertLine(pos, forward);
   InsertLine(pos, "{");
   InsertLine(pos, nspace);
   InsertLine(pos, EMPTY_STR);
   pos = Find(pos, forward);
   return Changed(pos);
}

//------------------------------------------------------------------------------

fixed_string PatchComment = "Overridden for patching.";
fixed_string PatchReturn = "void";
fixed_string PatchSignature = "Patch(sel_t selector, void* arguments)";
fixed_string PatchInvocation = "Patch(selector, arguments)";

word Editor::InsertPatch(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.InsertPatch");

   //  Extract the name of the function to insert.  (It will be "Patch",
   //  but this code might eventually be generalized for other functions.)
   //
   auto cls = static_cast< const Class* >(log.item_);
   auto name = log.GetNewFuncName();
   if(name.empty()) return Report("Log did not specify function name.");

   //  Find out where the function's declaration should be inserted and which
   //  file should contain its definition.  The definition is in another file,
   //  so access that file's editor and find out where the function should be
   //  defined in that file.
   //
   ItemDeclAttrs declAttrs(Cxx::Function, Cxx::Public);
   declAttrs.over_ = true;
   auto rc = FindItemDeclLoc(cls, name, declAttrs);
   if(rc != EditContinue) return Report(UnspecifiedFailure, rc);

   auto file = FindFuncDefnFile(cli, cls, name);
   if(file == nullptr) return NotFound("File for definition");

   auto& editor = file->GetEditor();

   ItemDefnAttrs defnAttrs(FuncOther);
   rc = editor.FindFuncDefnLoc(file, cls, name, defnAttrs);
   if(rc != EditContinue) return rc;

   //  Insert the function's declaration and definition.  The definition is
   //  inserted first because, if both are in the same file, the definition
   //  will follow the definition, so the position already found for it will
   //  become invalid if the declaration is inserted above it.
   //
   editor.InsertPatchDefn(cls, defnAttrs);
   InsertPatchDecl(declAttrs);
   auto pos = Find(defnAttrs.pos_, PatchSignature);
   return Changed(pos);
}

//------------------------------------------------------------------------------

void Editor::InsertPatchDecl(const ItemDeclAttrs& attrs)  //u
{
   Debug::ft("Editor.InsertPatchDecl");

   InsertBelowItemDecl(attrs);

   string code = PatchReturn;
   code.push_back(SPACE);
   code.append(PatchSignature);
   code.push_back(SPACE);
   code.append(OVERRIDE_STR);
   code.push_back(';');
   InsertLine(attrs.pos_, strCode(code, 1));

   InsertAboveItemDecl(attrs, PatchComment);
}

//------------------------------------------------------------------------------

void Editor::InsertPatchDefn(const Class* cls, const ItemDefnAttrs& attrs)  //u
{
   Debug::ft("Editor.InsertPatchDefn");

   InsertBelowItemDefn(attrs);

   InsertLine(attrs.pos_, "}");

   auto base = cls->BaseClass();
   auto code = base->Name();
   code.append(SCOPE_STR);
   code.append(PatchInvocation);
   code.push_back(';');
   InsertLine(attrs.pos_, strCode(code, 1));

   InsertLine(attrs.pos_, "{");

   code = PatchReturn;
   code.push_back(SPACE);
   code.append(cls->Name());
   code.append(SCOPE_STR);
   code.append(PatchSignature);
   InsertLine(attrs.pos_, code);

   InsertAboveItemDefn(attrs);
}

//------------------------------------------------------------------------------

word Editor::InsertPODCtor(const CodeWarning& log)
{
   Debug::ft("Editor.InsertPODCtor");

   //  Declare and define a constructor that initializes POD members to
   //  their default values.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

size_t Editor::InsertPrefix(size_t pos, const string& prefix)
{
   Debug::ft("Editor.InsertPrefix");

   auto first = LineFindFirst(pos);
   if(first == string::npos) return string::npos;

   if(pos + prefix.size() <= first)
   {
      Replace(pos, prefix.size(), prefix);
   }
   else
   {
      Insert(pos, prefix);
   }

   Changed();
   return pos;
}

//------------------------------------------------------------------------------

word Editor::InsertPureVirtual(const CodeWarning& log)
{
   Debug::ft("Editor.InsertPureVirtual");

   //  Insert a definition that invokes Debug::SwLog with strOver(this).
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

size_t Editor::InsertRule(size_t pos, char c)
{
   Debug::ft("Editor.InsertRule");

   string rule = COMMENT_STR;
   rule.append(LineLengthMax() - 2, c);
   InsertLine(pos, rule);
   return pos;
}

//------------------------------------------------------------------------------

word Editor::InsertSpecialFuncDecl  //u
   (CliThread& cli, const Class* cls, FunctionRole role)
{
   Debug::ft("Editor.InsertSpecialFuncDecl");

   ItemDeclAttrs declAttrs(Cxx::Function, Cxx::Public, role);
   auto rc = FindSpecialFuncDeclLoc(cli, cls, declAttrs);
   if(rc != EditContinue) return rc;

   auto code = spaces(declAttrs.indent_);
   auto className = cls->Name();

   string defn;

   if(!declAttrs.shell_)
   {
      defn += " = ";
      defn += (declAttrs.deleted_ ? DELETE_STR : DEFAULT_STR);
   }

   switch(declAttrs.role_)
   {
   case PureCtor:
      code += className + "()";
      break;

   case CopyCtor:
      code += className;
      code += "(const " + className + "& that)";
      break;

   case CopyOper:
      code += className + "& " + "operator=";
      code += "(const " + className + "& that)";
      break;

   case PureDtor:
      if(declAttrs.virt_) code += string(VIRTUAL_STR) + SPACE;
      code += '~' + className + "()";
      break;

   default:
      return Report("Unexpected special member function.");
   }

   code += defn;
   code.push_back(';');
   InsertBelowItemDecl(declAttrs);
   InsertLine(declAttrs.pos_, code);
   code.pop_back();

   auto comment = CreateSpecialFunctionComment(cls, declAttrs);
   InsertAboveItemDecl(declAttrs, comment);

   if(declAttrs.shell_)
   {
      auto file = FindFuncDefnFile(cli, cls, code);
      if(file == nullptr) return NotFound("File for function definition");

      auto& editor = file->GetEditor();

      ItemDefnAttrs defnAttrs(declAttrs.role_);
      rc = editor.FindFuncDefnLoc(file, cls, code, defnAttrs);
      if(rc != EditContinue) return rc;

      //  Insert the function's declaration and definition.
      //
      editor.InsertSpecialFuncDefn(cls, defnAttrs);
   }

   auto pos = source_.find(code, declAttrs.pos_);
   return Changed(pos);
}

//------------------------------------------------------------------------------

void Editor::InsertSpecialFuncDefn  //u
   (const Class* cls, const ItemDefnAttrs& attrs)
{
   Debug::ft("Editor.InsertSpecialFuncDefn");

   InsertBelowItemDefn(attrs);
   InsertLine(attrs.pos_, "}");

   auto code = spaces(IndentSize());
   code += COMMENT_STR;
   code += "* To be implemented.";
   InsertLine(attrs.pos_, code);
   InsertLine(attrs.pos_, "{");

   auto className = cls->Name();
   code.clear();

   if(attrs.role_ == CopyOper)
   {
      code = className;
      code += "& ";
   }

   code += className;
   code += SCOPE_STR;

   switch(attrs.role_)
   {
   case PureCtor:
      code += className + "()";
      break;

   case CopyCtor:
      code += className;
      code += "(const " + className + "& that)";
      break;

   case CopyOper:
      code += className + "& " + "operator=";
      code += "(const " + className + "& that)";
      break;

   case PureDtor:
      code += '~' + className + "()";
      break;
   }

   InsertLine(attrs.pos_, code);
   InsertAboveItemDefn(attrs);
}

//------------------------------------------------------------------------------

const size_t MaxRoleWarning = 15;

struct RoleWarning
{
   const FunctionRole role;  // type of special member function to add
   const Warning log;        // type of log that calls for its addition
};

//  Logs associated with adding a special member function.
//
const RoleWarning SpecialMemberFunctionLogs[MaxRoleWarning] =
{
   { PureCtor, ImplicitConstructor },
   { PureCtor, PublicConstructor },
   { PureCtor, ConstructorNotPrivate },
   { PureCtor, CtorCouldBeDeleted },
   { PureDtor, ImplicitDestructor },
   { PureDtor, NonVirtualDestructor },
   { PureDtor, DestructorNotPrivate },
   { CopyCtor, ImplicitCopyConstructor },
   { CopyCtor, RuleOf3DtorNoCopyCtor },
   { CopyCtor, RuleOf3CopyOperNoCtor },
   { CopyCtor, CopyCtorNotDeleted },
   { CopyOper, ImplicitCopyOperator },
   { CopyOper, RuleOf3DtorNoCopyOper },
   { CopyOper, RuleOf3CopyCtorNoOper },
   { CopyOper, CopyOperNotDeleted }
};

//  Maps a warning to the type of function associated with it.
//
FunctionRole WarningToRole(Warning log)
{
   for(size_t i = 0; i < MaxRoleWarning; ++i)
   {
      if(SpecialMemberFunctionLogs[i].log == log)
         return SpecialMemberFunctionLogs[i].role;
   }

   return FuncRole_N;
}

//------------------------------------------------------------------------------

word Editor::InsertSpecialFunctions(CliThread& cli, const CxxToken* item)
{
   Debug::ft("Editor.InsertSpecialFunctions");

   //  This coordinates adding all missing special member functions
   //  that can be defined as defaulted or deleted.
   //
   auto cls = static_cast< const Class* >(item);
   std::vector< CodeWarning* > logs;
   std::set< FunctionRole > roles;

   for(size_t i = 0; i < MaxRoleWarning; ++i)
   {
      auto log = FindLog(SpecialMemberFunctionLogs[i].log, item, 0);

      if((log != nullptr) && (log->status_ == NotFixed))
      {
         logs.push_back(log);
         roles.insert(SpecialMemberFunctionLogs[i].role);
      }
   }

   //  Handle the Rule of 3: if adding the destructor, copy constructor,
   //  or copy operator, add any of the others that are missing.  Don't,
   //  however, try to add a function that is deleted in a base class,
   //  and don't try to add a copy constructor or copy operator to a
   //  class derived from a base class for singletons.
   //
   if((roles.find(PureDtor) != roles.cend()) ||
      (roles.find(CopyCtor) != roles.cend()) ||
      (roles.find(CopyOper) != roles.cend()))
   {
      auto dtor = cls->FindFuncByRole(PureDtor, false);

      if(dtor == nullptr) roles.insert(PureDtor);

      if(!cls->HasSingletonBase())
      {
         auto copy = cls->FindFuncByRole(CopyCtor, true);
         auto oper = cls->FindFuncByRole(CopyOper, true);

         if(copy == nullptr)
            roles.insert(CopyCtor);
         else if((copy->GetClass() != cls) && !copy->IsDeleted())
            roles.insert(CopyCtor);

         if(oper == nullptr)
            roles.insert(CopyOper);
         else if((oper->GetClass() != cls) && !oper->IsDeleted())
            roles.insert(CopyOper);
      }
   }

   if(roles.size() > 1)
   {
      *cli.obuf << "This will also prompt to add related functions..." << CRLF;
      cli.Flush();
   }

   //  Add each function in ROLES and update the warnings associated with it.
   //
   for(int r = PureCtor; r < FuncOther; ++r)
   {
      auto role = static_cast< FunctionRole >(r);

      if(roles.find(role) != roles.cend())
      {
         roles.erase(role);
         auto rc = InsertSpecialFuncDecl(cli, cls, role);
         ReportFix(cli, nullptr, rc);

         if(rc == EditSucceeded)
         {
            for(size_t i = 0; i < logs.size(); ++i)
            {
               if(WarningToRole(logs[i]->warning_) == role)
                  logs[i]->status_ = Pending;
            }
         }
      }
   }

   return EditCompleted;
}

//------------------------------------------------------------------------------

word Editor::InsertUsing(const CodeWarning& log)  //u
{
   Debug::ft("Editor.InsertUsing");

   //  Create the new using statement and decide where to insert it.
   //
   string statement = USING_STR;
   statement.push_back(SPACE);
   statement += log.info_;
   statement.push_back(';');

   auto stop = CodeBegin();
   auto usings = false;

   for(auto pos = PrologEnd(); pos != string::npos; pos = NextBegin(pos))
   {
      if(CompareCode(pos, USING_STR) == 0)
      {
         //  If this using statement is alphabetically after STATEMENT,
         //  add the new statement before it.
         //
         usings = true;

         if(CompareCode(pos, statement) > 0)
         {
            InsertLine(pos, statement);
            return Changed(pos);
         }
      }
      else if((usings && IsBlankLine(pos)) || (pos >= stop))
      {
         //  We have now passed any existing using statements, so add
         //  the new statement here.  If it is the first one, set it
         //  off with blank lines.
         //
         if(!usings) InsertLine(pos, EMPTY_STR);
         InsertLine(pos, statement);
         if(!usings) InsertLine(pos, EMPTY_STR);
         return Changed(pos + 1);
      }
   }

   return Report("Failed to insert using statement.");
}

//------------------------------------------------------------------------------

size_t Editor::IntroStart(size_t pos, bool funcName) const
{
   Debug::ft("Editor.IntroStart");

   auto start = pos;
   auto found = false;

   for(auto curr = PrevBegin(pos); curr != string::npos; curr = PrevBegin(curr))
   {
      auto type = PosToType(curr);

      switch(type)
      {
      case EmptyComment:
      case TextComment:
      case SlashAsteriskComment:
         //
         //  These are attached to the code that follows, so include them.
         //
         start = curr;
         break;

      case BlankLine:
         //
         //  This can be included if it precedes or follows an fn_name.
         //
         if(!funcName) return start;
         if(found) start = curr;
         break;

      case FunctionName:
         if(!funcName) return start;
         found = true;
         start = curr;
         break;

      default:
         return start;
      }
   }

   return pos;
}

//------------------------------------------------------------------------------

size_t Editor::LineAfterItem(const CxxToken* item) const
{
   Debug::ft("Editor.LineAfterItem");

   size_t begin, end;
   if(!item->GetSpan2(begin, end)) return string::npos;
   return NextBegin(end);
}

//------------------------------------------------------------------------------

word Editor::MoveDefine(const CodeWarning& log)
{
   Debug::ft("Editor.MoveDefine");

   //  Move this #define directly after the #include directives.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

void Editor::MoveFuncDefn(FunctionVector& sorted, const Function* func)
{
   Debug::ft("Editor.MoveFuncDefn");

   Function* prev = nullptr;
   Function* next = nullptr;

   for(auto f = sorted.cbegin(); f != sorted.cend(); ++f)
   {
      if(FuncDefnsAreSorted(func, *f))
      {
         next = *f;
         break;
      }

      prev = *f;
   }

   //  Move FUNC so that it follows PREV and precedes NEXT.  Also move any
   //  items defined directly above FUNC and used by FUNC, which will cause
   //  compile errors (forward references) if not moved.
   //
   auto items = GetItemsForFuncDefn(func);

   string code;
   ItemDefnAttrs defnAttrs(func);
   UpdateItemDefnLoc(prev, func, next, defnAttrs);
   InsertBelowItemDefn(defnAttrs);
   auto from = CutCode(func, code);
   if(defnAttrs.pos_ > from) defnAttrs.pos_ -= code.size();
   auto dest = Paste(defnAttrs.pos_, code, from);
   InsertAboveItemDefn(defnAttrs);

   while(!items.empty())
   {
      auto item = items.back();
      items.pop_back();
      MoveItem(item, dest, prev);
   }
}

//------------------------------------------------------------------------------

void Editor::MoveItem(const CxxNamed* item, size_t& dest, const CxxScoped* prev)
{
   Debug::ft("Editor.MoveItem");

   //  Cut DECL and find ADJ's span.
   //
   string code;
   auto from = CutCode(item, code);

   if(from < dest)
   {
      dest -= code.size();
   }

   size_t begin, end;
   GetSpan(prev, begin, end);

   //  Add a blank line before and after ITEM if it is commented or if it is
   //  a non-trivial inline.  If PREV is commented, ensure that a blank line
   //  follows it.  If a comment begins at POS, ensure that a blank line
   //  precedes it.
   //
   auto blanks = (IsNonTrivialInline(item, code) ||
      (code.find_first_not_of(WhitespaceChars) == code.find('/')));

   if(blanks || CommentFollows(dest))
   {
      code.push_back(CRLF);
   }

   Paste(dest, code, from);

   if(blanks || CommentFollows(begin) || IsInItemGroup(prev))
   {
      InsertLine(dest, EMPTY_STR);
   }

   Changed();
}

//------------------------------------------------------------------------------

word Editor::MoveMemberInit(const CodeWarning& log)
{
   Debug::ft("Editor.MoveMemberInit");

   //  Move the member to the correct location in the initialization list.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

bool Editor::OverridesWereSorted(const CodeWarning& log) const
{
   Debug::ft("Editor.OverridesWereSorted");

   auto cls = log.item_->GetClass();

   for(auto w = warnings_.cbegin(); w != warnings_.cend(); ++w)
   {
      if((*w)->warning_ == OverrideNotSorted)
      {
         if(((*w)->status_ >= Pending) && ((*w)->item_->GetClass() == cls))
            return true;
      }
   }

   return false;
}

//------------------------------------------------------------------------------

fn_name Editor_Paste = "Editor.Paste";

size_t Editor::Paste(size_t pos, const std::string& code, size_t from)
{
   Debug::ft(Editor_Paste);

   if(from != lastCut_)
   {
      Debug::SwLog(Editor_Paste, "Illegal Paste operation: " + code, from);
      return string::npos;
   }

   source_.insert(pos, code);
   lastCut_ = string::npos;
   UpdatePos(Pasted, pos, code.size(), from);
   return pos;
}

//------------------------------------------------------------------------------

size_t Editor::PrologEnd() const
{
   Debug::ft("Editor.PrologEnd");

   for(size_t pos = 0; pos != string::npos; pos = NextBegin(pos))
   {
      if(LineTypeAttr::Attrs[PosToType(pos)].isCode) return pos;
   }

   return string::npos;
}

//------------------------------------------------------------------------------

void Editor::QualifyReferent(const CxxToken* item, CxxNamed* ref)
{
   Debug::ft("Editor.QualifyReferent");

   //  Within ITEM, prefix NS wherever SYMBOL appears as an identifier.
   //
   Namespace* ns = ref->GetSpace();
   auto symbol = &ref->Name();

   switch(ref->Type())
   {
   case Cxx::Namespace:
      ns = static_cast< Namespace* >(ref)->OuterSpace();
      break;
   case Cxx::Class:
      if(ref->IsInTemplateInstance())
      {
         auto tmplt = ref->GetTemplate();
         ns = tmplt->GetSpace();
         symbol = &tmplt->Name();
      }
   }

   auto nsName = ns->ScopedName(false);
   auto nsCode = nsName + SCOPE_STR;
   size_t pos, end;
   if(!item->GetSpan2(pos, end)) return;
   string name;

   while(FindIdentifier(pos, name, false) && (Curr() <= end))
   {
      if(name == *symbol)
      {
         //  Qualify NAME with NS if it is not already qualified.
         //
         if(source_.rfind(SCOPE_STR, pos) != (pos - strlen(SCOPE_STR)))
         {
            auto elem = file_->PosToItem(pos);
            auto qname = (elem != nullptr ? elem->GetQualName() : nullptr);
            if(qname != nullptr) qname->AddPrefix(nsName, ns);
            Insert(pos, nsCode);
            Changed();
            pos = NextPos(pos + nsCode.size());
         }
      }

      //  Look for the next name after the current one.
      //
      pos = NextPos(pos + name.size());
   }
}

//------------------------------------------------------------------------------

word Editor::RenameArgument(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.RenameArgument");

   //  This handles the following argument warnings:
   //  o AnonymousArgument: unnamed argument
   //  o DefinitionRenamesArgument: definition's name differs from declaration's
   //  o OverrideRenamesArgument: override's name differs from root's
   //
   auto func = static_cast< const Function* >(log.item_);
   auto index = func->LogOffsetToArgIndex(log.offset_);
   const Function* decl = func->GetDecl();
   const Function* defn = func->GetDefn();
   const Function* root = (func->IsOverride() ? func->FindRootFunc() : nullptr);

   //  Use the argument name from the root (if any), else the declaration,
   //  else the definition.
   //
   string argName;
   string defnName;
   auto declName = decl->GetArgs().at(index)->Name();
   if(defn != nullptr) defnName = defn->GetArgs().at(index)->Name();
   if(root != nullptr) argName = root->GetArgs().at(index)->Name();
   if(argName.empty()) argName = declName;
   if(argName.empty()) argName = defnName;
   if(argName.empty()) return NotFound("Candidate argument name");

   //  The declaration and definition are logged separately, so fix only
   //  the one that has a problem.
   //
   switch(log.warning_)
   {
   case AnonymousArgument:
   {
      auto pos = func->GetArgs().at(index)->GetTypeSpec()->GetPos();
      pos = FindFirstOf(",)", pos);
      if(pos == string::npos) return NotFound("End of argument");
      auto arg = func->GetArgs().at(index).get();
      arg->Rename(argName);
      argName.insert(0, 1, SPACE);
      Insert(pos, argName);
      return Changed(pos);
   }

   case DefinitionRenamesArgument:
      //
      //  See which name the user wants to use.
      //
      if(!declName.empty() && !defnName.empty())
      {
         argName = ChooseArgumentName(cli, declName, defnName);
         if(argName == defnName) func = decl;
      }
   };

   size_t begin, end;
   if(!func->GetSpan2(begin, end)) return NotFound("Function");
   if(func == decl) defnName = declName;
   auto& editor = func->GetFile()->GetEditor();

   auto arg = func->GetArgs().at(index).get();
   arg->ChangeName(argName);

   for(auto pos = editor.FindWord(begin, defnName); pos < end;
      pos = editor.FindWord(pos + defnName.size(), defnName))
   {
      auto item = file_->PosToItem(pos);
      if(item != nullptr) item->Rename(argName);
      editor.Replace(pos, defnName.size(), argName);
      end = end + argName.size() - defnName.size();
      editor.Changed();
   }

   return editor.Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::RenameDebugFtArgument(CliThread& cli, const CodeWarning& log)  //u
{
   Debug::ft("Editor.RenameDebugFtArgument");

   //  This handles the following warnings for the string passed to Debug::ft:
   //  o DebugFtNameMismatch: the string doesn't start with "Scope.Function"
   //  o DebugFtNameDuplicated: another function already uses the same string
   //
   //  Start by finding the location of the logged Debug::ft invocation.
   //
   auto cpos = Find(log.Pos(), "Debug::ft");
   if(cpos == string::npos) return NotFound("Debug::ft invocation");

   //  Find the location of the first fn_name definition that precedes this
   //  function.  If one is found, it belongs to a previous function if a
   //  right brace appears between it and the start of this function.
   //
   auto fpos = log.item_->GetPos();
   auto dpos = Rfind(fpos, "fn_name");

   if(dpos != string::npos)
   {
      auto valid = true;

      for(auto left = dpos; valid && (left < fpos); left = NextBegin(left))
      {
         if(LineFind(left, "}") != string::npos) valid = false;
      }

      if(!valid) dpos = string::npos;
   }

   //  Generate the string (FLIT) and fn_name (FVAR).  If FLIT is already
   //  in use, prompt the user for a unique suffix.
   //
   string flit, fvar, fname;

   DebugFtNames(static_cast< const Function* >(log.item_), flit, fvar);
   if(!EnsureUniqueDebugFtName(cli, flit, fname))
      return Report(FixSkipped);

   if(dpos == string::npos)
   {
      //  An fn_name definition was not found, so the Debug::ft invocation
      //  must have used a string literal.  Replace it.
      //
      auto lpar = FindFirstOf("(", cpos);
      if(lpar == string::npos) return NotFound("Left parenthesis");
      auto rpar = FindClosing('(', ')', lpar + 1);
      if(rpar == string::npos) return NotFound("Right parenthesis");
      Erase(lpar + 1, rpar - lpar - 1);
      Insert(lpar + 1, fname);
      return Changed(cpos);
   }

   //  The Debug::ft invocation used an fn_name.  It might be used elsewhere
   //  (e.g. for calls to Debug::SwLog), so keep its name and only replace
   //  its definition.
   //
   auto lpos = Find(dpos, QUOTE_STR);
   if(lpos == string::npos) return NotFound("fn_name left quote");
   auto rpos = source_.find(QUOTE, lpos + 1);
   if(rpos == string::npos) return NotFound("fn_name right quote");
   Replace(lpos, rpos - lpos + 1, fname);

   if(LineSize(lpos) - 1 > LineLengthMax())
   {
      auto epos = FindFirstOf("=", dpos);
      if(epos != string::npos) InsertLineBreak(epos + 1);
   }

   return Changed(lpos);
}

//------------------------------------------------------------------------------

word Editor::RenameIncludeGuard(const CodeWarning& log)
{
   Debug::ft("Editor.RenameIncludeGuard");

   //  This warning is logged against the #ifndef.
   //
   auto ifn = CurrBegin(log.Pos());
   if(ifn == string::npos) return NotFound("Position of #define");
   if(CompareCode(ifn, HASH_IFNDEF_STR) != 0) return NotFound(HASH_IFNDEF_STR);

   auto guard = log.File()->MakeGuardName();
   const_cast< CxxToken* >(log.item_)->Rename(guard);

   ifn += strlen(HASH_IFNDEF_STR) + 1;
   auto end = CurrEnd(ifn) - 1;
   Erase(ifn, end - ifn + 1);
   Insert(ifn, guard);

   auto def = Find(ifn, HASH_DEFINE_STR);
   if(def == string::npos) return NotFound(HASH_DEFINE_STR);
   def += strlen(HASH_DEFINE_STR) + 1;
   end = CurrEnd(def) - 1;
   Erase(def, end - def + 1);
   Insert(def, guard);
   return Changed(def);
}

//------------------------------------------------------------------------------

size_t Editor::Replace(size_t pos, size_t count, const std::string& code)
{
   Debug::ft("Editor.Replace");

   Erase(pos, count);
   Insert(pos, code);
   return pos;
}

//------------------------------------------------------------------------------

word Editor::ReplaceHeading(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceHeading");

   //  Remove the existing header and replace it with the standard one,
   //  inserting the file name where appropriate.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ReplaceName(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceName");

   //  Prompt for a new name that will replace the existing one.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ReplaceNull(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceNull");

   auto pos = log.item_->GetPos();
   if(pos == string::npos) return NotFound(NULL_STR, true);
   if(source_.compare(pos, sizeof(NULL_STR), NULL_STR) != 0)
      return NotFound(NULL_STR);
   Replace(pos, strlen(NULL_STR), NULLPTR_STR);
   const_cast< CxxToken* >(log.item_)->Rename(NULLPTR_STR);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::ReplaceSlashAsterisk(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceSlashAsterisk");

   auto pos0 = CurrBegin(log.Pos());
   if(pos0 == string::npos) return NotFound("Position of /*");
   auto pos1 = source_.find(COMMENT_BEGIN_STR, pos0);
   if(pos1 == string::npos) return NotFound(COMMENT_BEGIN_STR);
   auto pos2 = LineFind(pos1, COMMENT_END_STR);
   auto pos3 = LineFindNext(pos1 + strlen(COMMENT_BEGIN_STR));
   auto pos4 = (pos2 == string::npos ? string::npos :
      LineFindNext(pos2 + strlen(COMMENT_END_STR)));

   //  We now have
   //  o pos1: start of /*
   //  o pos2: start of */ (if on this line)
   //  o pos3: first non-blank following /* (if any)
   //  o pos4: first non-blank following */ (if any)
   //
   //  The scenarios are                                        pos2 pos3 pos4
   //  1: /*         erase /* and continue                        -    -    -
   //  2: /* aaa     replace /* with // and continue              -    *    -
   //  3: /* aaa */  replace /* with // and erase */ and return   *    *    -
   //  4: /* */ aaa  return                                       *    *    *
   //
   if(pos4 != string::npos)  // [4]
   {
      return Report("Unchanged: code follows /*...*/");
   }
   else if(pos3 == string::npos)  // [1]
   {
      Erase(pos1, strlen(COMMENT_BEGIN_STR));
      Changed();
   }
   else if((pos2 == string::npos) && (pos3 != string::npos))  // [2]
   {
      source_.replace(pos1, strlen(COMMENT_BEGIN_STR), COMMENT_STR);
      Changed();
   }
   else  // [3]
   {
      Erase(pos2, strlen(COMMENT_END_STR));
      source_.replace(pos1, strlen(COMMENT_BEGIN_STR), COMMENT_STR);
      return Changed(pos1);
   }

   //  Subsequent lines will be commented with "//", which will be indented
   //  appropriately and followed by two spaces.
   //
   auto info = GetLineInfo(pos0);
   auto comment = spaces(info->depth * IndentSize());
   comment += COMMENT_STR + spaces(2);

   //  Now continue with subsequent lines:
   //  o pos0: start of the current line
   //  o pos2: start of */ (if on this line)
   //  o pos3: first non-blank after start of line and preceding */
   //  o pos4: first non-blank following */ (if any)
   //
   //  The scenarios are                                    pos2 pos3 pos4
   //  1: no */       prefix // and continue                  -
   //  2: */          erase */ and return                     *    -    -
   //  3: */ aaa      replace */ with line break and return   *    -    *
   //  4: aaa */      prefix // and erase */ and return       *    *    -
   //  5: aaa */ aaa  prefix // and replace */ with line      *    *    *
   //                 break and return
   //
   for(pos0 = NextBegin(pos0); pos0 != string::npos; pos0 = NextBegin(pos0))
   {
      pos2 = LineFind(pos0, COMMENT_END_STR);
      pos3 = LineFindNext(pos0);
      if(pos3 == pos2) pos3 = string::npos;

      if(pos2 != string::npos)
         pos4 = LineFindNext(pos2 + strlen(COMMENT_END_STR));
      else
         pos4 = string::npos;

      if(pos2 == string::npos)  // [1]
      {
         InsertPrefix(pos0, comment);
         Changed();
      }
      else if((pos3 == string::npos) && (pos4 == string::npos))  // [2]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         return Changed(PrevBegin(pos2));
      }
      else if((pos3 == string::npos) && (pos4 != string::npos))  // [3]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         Changed();
         InsertLineBreak(pos2);
         return Changed(PrevBegin(pos2));
      }
      else if((pos3 != string::npos) && (pos4 == string::npos))  // [4]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         InsertPrefix(pos0, comment);
         return Changed(pos0);
      }
      else  // [5]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         InsertLineBreak(pos2);
         InsertPrefix(pos0, comment);
         return Changed(pos0);
      }
   }

   return Report("Closing */ not found. Inspect changes!");
}

//------------------------------------------------------------------------------

word Editor::ReplaceUsing(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceUsing");

   //  Before removing the using statement, add type aliases to each class
   //  for symbols that appear in its definition and that were resolved by
   //  a using statement.
   //
   ResolveUsings();
   return EraseItem(log.item_);
}

//------------------------------------------------------------------------------
//
//  Displays EXPL when RC was returned after fixing LOG.
//
void Editor::ReportFix(CliThread& cli, CodeWarning* log, word rc)
{
   Debug::ft("Editor.ReportFix");

   auto expl = GetExpl();

   if(rc < EditCompleted)
   {
      *cli.obuf << spaces(2) << (expl.empty() ? SuccessExpl : expl);
      if(expl.empty() || (expl.back() != CRLF)) *cli.obuf << CRLF;
      cli.Flush();
   }

   if((log != nullptr) && (rc == EditSucceeded)) log->status_ = Pending;
}

//------------------------------------------------------------------------------

void Editor::ReportFixInFile(CliThread& cli, CodeWarning* log, word rc) const
{
   Debug::ft("Editor.ReportFixInFile");

   auto fn = file_->Name();
   auto expl = GetExpl();

   if(expl.empty())
   {
      fn += ": ";
      expl = fn + SuccessExpl;
   }
   else
   {
      fn += ":\n" + spaces(4);
      expl = fn + expl;
   }

   SetExpl(expl);
   ReportFix(cli, log, rc);
}

//------------------------------------------------------------------------------

word Editor::ResolveUsings()
{
   Debug::ft("Editor.ResolveUsings");

   //  This function only needs to run once per file.
   //
   if(aliased_) return EditSucceeded;

   auto& items = file_->Items();

   for(auto item = items.begin(); item != items.end(); ++item)
   {
      auto refs = FindUsingReferents(*item);

      for(auto ref = refs.cbegin(); ref != refs.cend(); ++ref)
      {
         QualifyReferent(*item, *ref);
      }
   }

   aliased_ = true;
   return EditSucceeded;
}

//------------------------------------------------------------------------------

void Editor::Setup(CodeFile* file)
{
   Debug::ft("Editor.Setup");

   if((file_ != nullptr) || (file == nullptr)) return;

   //  Get the file's code and configure the lexer.  The code is read in
   //  again because Lexer.Preprocess has modified the original version.
   //
   file_ = file;
   file_->ReadCode(source_);
   Initialize(source_, file_);
   CalcLineTypes(false);
   CalcDepths();

   //  Get the file's warnings and sort them for fixing.  The order reduces
   //  the chances of an item's position changing before it is edited.
   //
   CodeWarning::GetWarnings(file_, warnings_);
   std::sort(warnings_.begin(), warnings_.end(), CodeWarning::IsSortedToFix);
}

//------------------------------------------------------------------------------

word Editor::SortFunctions(const CodeWarning& log)
{
   Debug::ft("Editor.SortFunctions");

   //  Start by getting the functions defined in our file, sorted by position.
   //  Extract the functions in the AREA associated with the log; all of them
   //  will be sorted together.  Step through the file until the first function
   //  in AREA is reached.  Each of its functions will end up in one of three
   //  groups:
   //  o SORTED: functions that don't need to move
   //  o UNSORTED: functions that need to move
   //  o ORPHANS: functions located *after* the first function in another area;
   //    all of these also need to move
   //
   auto defns = file_->GetFuncDefnsToSort();
   auto area = log.item_->GetArea();
   auto reached = false;
   auto orphans = FuncsInArea(defns, area);
   FunctionVector sorted;
   FunctionVector unsorted;
   Function* prev = nullptr;

   for(auto f = defns.cbegin(); f != defns.cend(); ++f)
   {
      if((*f)->GetArea() == area)
      {
         reached = true;

         if((prev == nullptr) || FuncDefnsAreSorted(prev, *f))
         {
            prev = *f;
            sorted.push_back(*f);
         }
         else
         {
            //  When functions were previously sorted, re-sorting is usually
            //  required only because of name changes of the addition of new
            //  functions.  In this case, when f2 and f3 are missorted in the
            //  sequence f1-f2-f3, cut and paste operations can be reduced by
            //  moving f2, provided that f3 follows f1.  Here, that means
            //  moving PREV from SORTED to UNSORTED and adding *f to SORTED.
            //
            auto size = sorted.size();

            if((size > 1) && FuncDefnsAreSorted(sorted[size - 2], *f))
            {
               unsorted.push_back(prev);
               sorted.pop_back();
               prev = *f;
               sorted.push_back(*f);
            }
            else
            {
               unsorted.push_back(*f);
            }
         }

         CodeTools::EraseItem(orphans, *f);
      }
      else
      {
         if(reached) break;
      }
   }

   if(!reached) return NotFound("Unsorted function");

   //  A function in AREA was found, and now we've reached a function in a
   //  different area.  Add any functions in ORPHANS to UNSORTED, and then
   //  move each unsorted function to its correct position.
   //
   for(auto o = orphans.cbegin(); o != orphans.cend(); ++o)
   {
      unsorted.push_back(*o);
   }

   while(!unsorted.empty())
   {
      auto func = unsorted.back();
      unsorted.pop_back();
      MoveFuncDefn(sorted, func);
      sorted.push_back(func);
      std::sort(sorted.begin(), sorted.end(), IsSortedByPos);
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::SortIncludes()
{
   Debug::ft("Editor.SortIncludes");

   //  Sort our file's #include directives.  This doesn't actually move their
   //  code, so each one retains its pre-sorted position.  This allows us to
   //  then selection sort them.  Find each successive #include, and if its
   //  position doesn't match the next #include in the list, cut the correct
   //  #include and paste it in front of the current one.
   //
   file_->SortIncludes();

   auto& includes = file_->Includes();
   auto incl = includes.cbegin();

   for(auto pos = IncludesBegin(); pos != string::npos; pos = NextBegin(pos))
   {
      if(CompareCode(pos, HASH_INCLUDE_STR) == 0)
      {
         if(file_->PosToItem(pos) != incl->get())
         {
            auto from = (*incl)->GetPos();
            auto code = GetCode(from);
            from = EraseLine(from);
            Paste(pos, code, from);
         }

         if(++incl == includes.cend()) break;
      }
   }

   sorted_ = true;
   Changed();
   return Report("All #includes in this file sorted.", EditSucceeded);
}

//------------------------------------------------------------------------------

word Editor::SortOverrides(const CodeWarning& log)
{
   Debug::ft("Editor.SortOverrides");

   //  Get the functions for the class associated with LOG, sorted by position.
   //  Within each access control, find all overrides and the LAST function that
   //  is not an override.  Sort the overrides alphabetically and move them so
   //  that they follow LAST.  A function that shares a comment with other items
   //  is not moved, even if it is an override.
   //
   auto cls = log.item_->GetClass();
   auto funcs = cls->GetFunctions();
   Function* last = nullptr;
   FunctionVector overrides;
   size_t index = 0;
   size_t pos = string::npos;

   while(index < funcs.size())
   {
      auto acc = funcs[index]->GetAccess();

      do
      {
         auto func = funcs[index];
         if(func->GetAccess() != acc) break;

         if(IsInItemGroup(func))
            last = func;
         else if(!func->IsOverride())
            last = func;
         else
            overrides.push_back(func);
      }
      while(++index < funcs.size());

      if(!overrides.empty())
      {
         //  Find POS, the insertion point when moving overrides.  If all
         //  of the functions were overrides (LAST is nullptr), POS is at
         //  the start of the first override.  If a non-override was found
         //  (LAST is not nullptr), POS follows LAST.
         //
         if(last == nullptr)
         {
            size_t end;
            GetSpan(overrides.front(), pos, end);
         }
         else
         {
            size_t begin;
            GetSpan(last, begin, pos);
            pos = NextBegin(pos);
         }

         //  Sort the overrides and then cut and paste each one to POS.
         //
         std::sort(overrides.begin(), overrides.end(), IsSortedByName);

         for(size_t i = overrides.size() - 1; i != SIZE_MAX; --i)
         {
            MoveItem(overrides[i], pos, last);
         }

         overrides.clear();
      }

      last = nullptr;
   }

   return Report("All overrides in this class sorted.", EditSucceeded);
}

//------------------------------------------------------------------------------

word Editor::SplitVirtualFunction(const Function* func)
{
   Debug::ft("Editor.SplitVirtualFunction");

   //  Split this public virtual function:
   //  o Rename the function, its overrides, and its invocations within
   //    overrides to its original name + "_v".
   //  o Make its declaration protected and virtual.
   //  o Make its public declaration non-virtual, with the implementation
   //    simply invoking its renamed, protected version.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::TagAsConstArgument(const Function* func, word offset)
{
   Debug::ft("Editor.TagAsConstArgument");

   //  Find the line on which the argument's type appears, and insert
   //  "const" before that type.
   //
   auto index = func->LogOffsetToArgIndex(offset);
   auto type = func->GetArgs().at(index)->GetTypeSpec();
   auto pos = type->GetPos();
   if(pos == string::npos) return NotFound("Argument type");
   Insert(pos, "const ");
   type->Tags()->SetConst(true);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstData(const Data* data)
{
   Debug::ft("Editor.TagAsConstData");

   //  Insert "const" before the data declaration's type.
   //
   auto type = data->GetTypeSpec();
   auto pos = type->GetPos();
   if(pos == string::npos) return NotFound("Data type");
   Insert(pos, "const ");
   type->Tags()->SetConst(true);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstFunction(const Function* func)
{
   Debug::ft("Editor.TagAsConstFunction");

   //  Insert " const" after the right parenthesis at the end of the function's
   //  argument list.
   //
   auto endsig = FindArgsEnd(func);
   if(endsig == string::npos) return NotFound("End of argument list");
   Insert(endsig + 1, " const");
   const_cast< Function* >(func)->SetConst(true);
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstPointer(const Data* data)
{
   Debug::ft("Editor.TagAsConstPointer");

   //  If there is more than one pointer, this applies to the last one, so
   //  back up from the data item's name.  Search for the name on this line
   //  in case other edits have altered its position.
   //
   auto type = data->GetTypeSpec();
   auto name = FindWord(data->GetPos(), data->Name());
   if(name == string::npos) return NotFound("Member name");
   auto ptr = Rfind(name, "*");
   if(ptr == string::npos) return NotFound("Pointer tag");
   Insert(ptr + 1, " const");
   type->Tags()->SetConstPtr();
   return Changed(ptr);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstReference(const Function* func, word offset)
{
   Debug::ft("Editor.TagAsConstReference");

   //  Find the line on which the argument's name appears.  Insert a reference
   //  tag before the argument's name and "const" before its type.  The tag is
   //  added first so that its position won't change as a result of adding the
   //  "const" earlier in the line.
   //
   auto& args = func->GetArgs();
   auto index = func->LogOffsetToArgIndex(offset);
   auto arg = args.at(index).get();
   if(arg == nullptr) return NotFound("Argument");
   auto pos = arg->GetPos();
   if(pos == string::npos) return NotFound("Argument name");
   auto prev = RfindNonBlank(pos - 1);
   Insert(prev + 1, "&");
   arg->GetTypeSpec()->Tags()->SetRefs(1);
   auto rc = TagAsConstArgument(func, offset);
   if(rc != EditSucceeded) return rc;
   return Changed(prev);
}

//------------------------------------------------------------------------------

word Editor::TagAsDefaulted(const Function* func)
{
   Debug::ft("Editor.TagAsDefaulted");

   //  If this is a separate definition, delete it.
   //
   if(func->GetDecl() != func)
   {
      return EraseItem(func);
   }

   //  This is the function's declaration, and possibly its definition.
   //
   auto endsig = FindSigEnd(func);
   if(endsig == string::npos) return NotFound("Signature end");
   if(source_[endsig] == ';')
   {
      Insert(endsig, " = default");
   }
   else
   {
      auto right = FindFirstOf("}", endsig + 1);
      if(right == string::npos) return NotFound("Right brace");
      Erase(endsig, right - endsig + 1);
      Insert(endsig, "= default;");
   }

   const_cast< Function* >(func)->SetDefaulted();
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::TagAsExplicit(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsExplicit");

   //  A constructor can be tagged "constexpr", which the parser looks for
   //  only *after* "explicit".
   //
   auto ctor = log.item_->GetPos();
   auto prev = LineRfind(ctor, CONSTEXPR_STR);
   if(prev != string::npos) ctor = prev;
   Insert(ctor, "explicit ");
   ((Function*) log.item_)->SetExplicit(true);
   return Changed(ctor);
}

//------------------------------------------------------------------------------

word Editor::TagAsNoexcept(const Function* func)
{
   Debug::ft("Editor.TagAsNoexcept");

   //  Insert "noexcept" after "const" but before "override" or "final".
   //  Start by finding the end of the function's argument list.
   //
   auto rpar = FindArgsEnd(func);
   if(rpar == string::npos) return NotFound("End of argument list");

   //  If there is a "const" after the right parenthesis, insert "noexcept"
   //  after it, else insert "noexcept" after the right parenthesis.
   //
   auto cons = FindNonBlank(rpar + 1);
   if(CompareCode(cons, CONST_STR) == 0)
   {
      Insert(cons + strlen(CONST_STR), " noexcept");
      return Changed(cons);
   }

   Insert(rpar + 1, " noexcept");
   const_cast< Function* >(func)->SetNoexcept(true);
   return Changed(rpar);
}

//------------------------------------------------------------------------------

word Editor::TagAsOverride(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsOverride");

   //  Remove the function's virtual tag, if any.
   //
   auto rc = EraseVirtualTag(log);
   if(rc != EditSucceeded) return rc;

   //  Insert "override" after the last non-blank character at the end
   //  of the function's signature.
   //
   auto endsig = FindSigEnd(log);
   if(endsig == string::npos) return NotFound("Signature end");
   endsig = RfindNonBlank(endsig - 1);
   Insert(endsig + 1, " override");
   ((Function*) log.item_)->SetOverride(true);
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::TagAsStaticFunction(const Function* func)
{
   Debug::ft("Editor.TagAsStaticFunction");

   //  Start with the function's return type in case it's on the line above
   //  the function's name.  Then find the end of the argument list.
   //
   auto type = func->GetTypeSpec()->GetPos();
   if(type == string::npos) return NotFound("Function type");
   auto rpar = FindArgsEnd(func);
   if(rpar == string::npos) return NotFound("End of argument list");

   //  Only a function's declaration, not its definition, is tagged "static".
   //  Start by removing any "virtual" tag.  Then put "static" after "extern"
   //  and/or "inline".
   //
   if(func->GetDecl() == func)
   {
      auto front = LineRfind(type, VIRTUAL_STR);
      if(front != string::npos) Erase(front, strlen(VIRTUAL_STR) + 1);
      front = LineRfind(type, INLINE_STR);
      if(front != string::npos) type = front;
      front = LineRfind(type, EXTERN_STR);
      if(front != string::npos) type = front;
      Insert(type, "static ");
      const_cast< Function* >(func)->SetStatic(true, Cxx::NIL_OPERATOR);
      Changed();
   }

   //  A static function cannot be const, so remove that tag if it exists.  If
   //  "const" is on the same line as RPAR, delete any space *before* "const";
   //  if it's on the next line, delete any space *after* "const" to preserve
   //  indentation.
   //
   if(func->IsConst())
   {
      auto tag = FindWord(rpar, CONST_STR);

      if(tag != string::npos)
      {
         Erase(tag, strlen(CONST_STR));
         const_cast< Function* >(func)->SetConst(false);

         if(OnSameLine(rpar, tag))
         {
            if(IsBlank(source_[tag - 1])) Erase(tag - 1, 1);
         }
         else
         {
            if(IsBlank(source_[tag])) Erase(tag, 1);
         }
      }
   }

   return Changed(type);
}

//------------------------------------------------------------------------------

word Editor::TagAsVirtual(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsVirtual");

   //  Make this destructor virtual.
   //
   auto pos = Insert(log.item_->GetPos(), "virtual ");
   ((Function*) log.item_)->SetVirtual(true);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::UpdateItemControls(const Class* cls, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.UpdateItemControls");

   //  Determine the access control that will be in effect at attrs.pos_.
   //  If we reach an access control that is more restrictive than the
   //  one for the item, insert the item directly above it.
   //
   size_t begin, end;
   if(!cls->GetSpan2(begin, end))
   {
      return NotFound("Item's class");
   }

   auto prev = cls->DefaultAccess();

   for(auto pos = CurrBegin(begin); pos <= attrs.pos_; pos = NextBegin(pos))
   {
      auto code = GetCode(pos);
      auto acc = FindAccessControl(code);

      if(acc != Cxx::Access_N)
      {
         if(acc < attrs.access_)
         {
            attrs.pos_ = pos;
            attrs.thisctrl_ = (prev != attrs.access_);
            if(attrs.thisctrl_) attrs.blank_ = BlankNone;
            if(attrs.blank_ == BlankBelow) attrs.blank_ = BlankAbove;
            return EditContinue;
         }

         prev = acc;
      }
   }

   if(prev != attrs.access_)
   {
      //  The item's access control is not in effect at the insertion point,
      //  so it must be inserted, and the access control that was in effect
      //  must be inserted after the item unless we've reached the end of
      //  the class.
      //
      attrs.thisctrl_ = true;
      if(attrs.blank_ == BlankAbove) attrs.blank_ = BlankBelow;
      if(PosToType(attrs.pos_) != CloseBraceSemicolon) attrs.nextctrl_ = prev;
   }
   else
   {
      //  If the item is being insert *at* its access control, insert it
      //  immediately after that control.
      //
      if(FindAccessControl(GetCode(attrs.pos_)) == attrs.access_)
      {
         attrs.pos_ = NextBegin(attrs.pos_);
         if(attrs.blank_ == BlankAbove) attrs.blank_ = BlankBelow;
      }
   }

   return EditContinue;
}

//------------------------------------------------------------------------------

word Editor::UpdateItemDeclAttrs
   (const CxxToken* item, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.UpdateItemDeclAttrs");

   if(item == nullptr) return EditContinue;

   size_t begin, end;
   if(!item->GetSpan2(begin, end)) return NotFound("Adjacent item");

   //  Indent the code to match that of ITEM.
   //
   attrs.indent_ = LineFindFirst(begin) - CurrBegin(begin);

   //  If ITEM is commented, a blank line should also precede or follow it.
   //
   auto type = PosToType(PrevBegin(begin));
   auto& line = LineTypeAttr::Attrs[type];

   if(!line.isCode && (type != BlankLine))
   {
      attrs.comment_ = true;
      attrs.blank_ = BlankAbove;
      return EditContinue;
   }

   //  ITEM isn't commented, so see if a blank line precedes and follows it.
   //
   if((type == BlankLine) && (PosToType(NextBegin(end)) == BlankLine))
   {
      attrs.blank_ = BlankAbove;
   }

   return EditContinue;
}

//------------------------------------------------------------------------------

word Editor::UpdateItemDeclLoc(const Class* cls,
   const CxxToken* prev, const CxxToken* next, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.UpdateItemDeclLoc");

   //  PREV and NEXT are the items that precede and follow the item whose
   //  declaration is to be inserted.
   //
   auto rc = UpdateItemDeclAttrs(prev, attrs);
   if(rc != EditContinue) return rc;
   rc = UpdateItemDeclAttrs(next, attrs);
   if(rc != EditContinue) return rc;

   //  Special member functions are added in order of FunctionRole, so add
   //  them bottom-up so that they will appear in the desired order.
   //
   if(next == nullptr)
   {
      //  Insert the item before the class's final closing brace.
      //
      size_t begin, end;
      if(!cls->GetSpan2(begin, end)) return NotFound("Item's class");
      attrs.pos_ = CurrBegin(end);
      if((prev == nullptr) && (next == nullptr)) attrs.comment_ = true;
      if(attrs.blank_ == BlankBelow) attrs.blank_ = BlankAbove;
      return UpdateItemControls(cls, attrs);
   }

   //  Insert the item before NEXT.  If the item is to be set off with a
   //  blank, the blank must follow it.
   //
   if(attrs.blank_ != BlankNone) attrs.blank_ = BlankBelow;

   auto pred = PrevBegin(next->GetPos());

   while(true)
   {
      auto type = PosToType(pred);

      if(!LineTypeAttr::Attrs[type].isCode)
      {
         if(type == BlankLine) break;

         //  This is a comment, so keep moving up to find the insertion point.
         //
         pred = PrevBegin(pred);
         continue;
      }

      if(type == AccessControl)
      {
         if(FindAccessControl(GetCode(pred)) != attrs.access_)
         {
            //  This isn't the desired control.  Insert the item here or
            //  above; UpdateItemControls will set the exact position.
            //
            attrs.pos_ = pred;
            return UpdateItemControls(cls, attrs);
         }

         //  This is the desired access control.  Insert the item after
         //  it so that it can be reused.
         //
         attrs.pos_ = NextBegin(pred);
         return EditContinue;
      }

      break;
   }

   //  Insert the item above the line that follows PRED.
   //
   attrs.pos_ = NextBegin(pred);
   return UpdateItemControls(cls, attrs);
}

//------------------------------------------------------------------------------

void Editor::UpdateItemDefnAttrs(const CxxToken* prev,
   const CxxToken* item, const CxxToken* next, ItemDefnAttrs& attrs) const
{
   Debug::ft("Editor.UpdateItemDefnAttrs");

   auto prevOffsets = GetOffsets(prev);
   auto itemOffsets = GetOffsets(item);
   auto nextOffsets = GetOffsets(next);

   if(item == nullptr)
   {
      //  ITEM doesn't exist, so PREV and NEXT determine the offsets.
      //
      if(prev != nullptr)
      {
         //  The item will be added below PREV, so duplicate the offset
         //  currently below PREV, which will get pushed below the item.
         //
         itemOffsets.above_ = prevOffsets.below_;
      }
      else if(next != nullptr)
      {
         //  The item will be added above NEXT, so duplicate the offset
         //  currently above NEXT,which will get pushed above the item.
         //
         itemOffsets.below_ = nextOffsets.above_;
      }
      else
      {
         //  The item will be added at the end of the file.  There must be
         //  *something* in the file, so add a rule above the item.
         //
         itemOffsets.above_ = OffsetRule;
      }
   }
   else
   {
      //  ITEM already exists, so the offsets above and below it are known.
      //
      if(prev != nullptr)
      {
         //  The offset below PREV will be pushed below ITEM, so clear the
         //  offset below ITEM unless it is greater.  An offset above ITEM
         //  has to be inserted, so use the larger one.
         //
         if(prevOffsets.below_ >= itemOffsets.below_)
            itemOffsets.below_ = OffsetNone;
         if(itemOffsets.above_ < prevOffsets.below_)
            itemOffsets.above_ = prevOffsets.below_;
      }
      else if(next != nullptr)
      {
         //  The offset above NEXT will be pushed above ITEM, so clear the
         //  offset above ITEM unless it is greater.  An offset below ITEM
         //  has to be inserted, so use the larger one.
         //
         if(nextOffsets.above_ >= itemOffsets.above_)
            itemOffsets.above_ = OffsetNone;
         if(itemOffsets.below_ < nextOffsets.above_)
            itemOffsets.below_ = nextOffsets.above_;
      }
   }

   attrs.offsets_.above_ = itemOffsets.above_;
   attrs.offsets_.below_ = itemOffsets.below_;
}

//------------------------------------------------------------------------------

word Editor::UpdateItemDefnLoc(const CxxToken* prev,
   const CxxToken* item, const CxxToken* next, ItemDefnAttrs& attrs) const
{
   Debug::ft("Editor.UpdateItemDefnLoc");

   //  If PREV is NULLPTR and NEXT isn't, ITEM will be inserted before NEXT.
   //  If NEXT is a function that uses items directly above it, insert ITEM
   //  above those items, which are probably specific to NEXT.
   //
   if((prev == nullptr) && (next != nullptr) && (next->Type() == Cxx::Function))
   {
      auto items = GetItemsForFuncDefn(static_cast< const Function* >(next));
      if(!items.empty()) next = items.front();
   }

   UpdateItemDefnAttrs(prev, item, next, attrs);

   if(prev != nullptr)
   {
      //  Insert the item after the one that precedes it.
      //
      attrs.pos_ = LineAfterItem(prev);
      return EditContinue;
   }

   if(next == nullptr)
   {
      //  Insert the item at the bottom of the file.  If it ends with two
      //  closing braces, the second one ends a namespace definition, so
      //  insert the definition above that one.
      //
      auto pos = PrevBegin(string::npos);
      auto type = PosToType(pos);
      if(type != CloseBrace) return NotFound("File's second last }");
      type = PosToType(PrevBegin(pos));
      if(type != CloseBrace) return NotFound("File's last }");
      attrs.pos_ = pos;
      return EditContinue;
   }

   //  Insert the item before NEXT and any blank lines which precede it.
   //
   auto pred = PrevBegin(next->GetPos());

   while(pred != string::npos)
   {
      if(PosToType(pred) == BlankLine)
      {
         pred = PrevBegin(pred);
      }
      else
      {
         attrs.pos_ = NextBegin(pred);
         return EditContinue;
      }
   }

   attrs.pos_ = 0;
   return EditContinue;
}

//------------------------------------------------------------------------------

void Editor::UpdatePos
   (EditorAction action, size_t begin, size_t count, size_t from)
{
   Debug::ft("Editor.UpdatePos");

   Update();
   file_->UpdatePos(action, begin, count, from);

   for(auto w = warnings_.begin(); w != warnings_.end(); ++w)
   {
      (*w)->UpdatePos(action, begin, count, from);
   }

   Changed();
}

//------------------------------------------------------------------------------

word Editor::Write()
{
   Debug::ft("Editor.Write");

   std::ostringstream stream;

   //  Create a new file to hold the reformatted version.
   //
   auto path = file_->Path();
   auto temp = path + ".tmp";
   auto output = SysFile::CreateOstream(temp.c_str(), true);
   if(output == nullptr)
   {
      stream << "Failed to open output file for " << file_->Name();
      return Report(stream, EditAbort);
   }

   *output << source_;

   //  Delete the original file and replace it with the new one.
   //
   output.reset();
   auto err = remove(path.c_str());

   if(err != 0)
   {
      stream << "Failed to remove " << file_->Name() << ": error=" << err;
      return Report(stream, EditAbort);
   }

   err = rename(temp.c_str(), path.c_str());

   if(err != 0)
   {
      stream << "Failed to rename " << file_->Name() << ": error=" << err;
      return Report(stream, EditAbort);
   }

   for(auto w = warnings_.begin(); w != warnings_.end(); ++w)
   {
      if((*w)->status_ == Pending) (*w)->status_ = Fixed;
   }

   stream << "..." << file_->Name() << " committed";
   return Report(stream, EditCompleted);
}
}
