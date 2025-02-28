//==============================================================================
//
//  CodeWarning.h
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
#ifndef CODEWARNING_H_INCLUDED
#define CODEWARNING_H_INCLUDED

#include <cstddef>
#include <iosfwd>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "CodeTypes.h"
#include "CxxFwd.h"
#include "CxxLocation.h"
#include "LibraryItem.h"
#include "LibraryTypes.h"
#include "SysTypes.h"

using NodeBase::fixed_string;
using NodeBase::word;

//------------------------------------------------------------------------------

namespace CodeTools
{
//  Attributes of a Warning.
//
struct WarningAttrs
{
   //  Set if the editor can fix the warning.
   //
   const bool fixable;

   //  A string that explains the warning.
   //
   fixed_string expl;

   //  Constructs a warning with the specified attributes.
   //
   WarningAttrs(bool fix, fixed_string expl);
};

//------------------------------------------------------------------------------
//
//  Inserts a string for WARNING into STREAM.
//
std::ostream& operator<<(std::ostream& stream, Warning warning);

//  Whether a warning has been fixed.
//
enum WarningStatus
{
   NotSupported,  // editor does not support fixing this warning
   Nullified,     // item associated with warning was deleted
   NotFixed,      // code not changed
   Pending,       // code changed but not written to file
   Fixed          // code changed and written to file
};

//------------------------------------------------------------------------------
//
//  Used to log a warning.
//
class CodeWarning
{
   friend class Editor;
public:
   //  Almost every member is supplied to the constructor.  If POS is
   //  string::npos, it means that the warning has no code to display.
   //
   CodeWarning(Warning warning, CodeFile* file, size_t pos,
      const CxxToken* item, word offset, const std::string& info);

   //  Not subclassed.
   //
   ~CodeWarning() = default;

   //  Copy constructor.  Needed to allow sorting.
   //
   CodeWarning(const CodeWarning& that) = default;

   //  Copy operator.  Needed to allow sorting.
   //
   CodeWarning& operator=(const CodeWarning& that) = default;

   //  Returns the warning.
   //
   Warning GetWarning() const { return warning_; }

   //  Returns the file in which the warning appeared.
   //
   CodeFile* File() const { return loc_.GetFile(); }

   //  Returns the position at which the warning appeared.
   //
   size_t Pos() const { return loc_.GetPos(); }

   //  Returns the line number on which the warning appeared.
   //
   size_t Line() const;

   //  Adds the log to the global set of warnings unless it is a duplicate
   //  or suppressed.
   //
   void Insert() const;

   //  Initializes the Attrs map.
   //
   static void Initialize();

   //  Adds N to the number of line types of type T.
   //
   static void AddLineType(LineType t, size_t n) { LineTypeCounts_[t] += n; }

   //  Returns the explanation for warning W.
   //
   static fixed_string Expl(Warning w) { return Attrs_.at(w).expl; }

   //  Returns true if FILE was logged for WARNING.
   //
   static bool HasWarning(const CodeFile* file, Warning warning);

   //  Generates a report in STREAM for FILES.  The report includes line
   //  type counts and warnings found during parsing and compilation.
   //
   static void GenerateReport(std::ostream* stream, const LibItemSet& files);

   //  Nullifies warnings associated with ITEM, which was deleted.
   //
   static void ItemDeleted(const CxxToken* item);
private:
   //  Comparison operators.
   //
   bool operator==(const CodeWarning& that) const;
   bool operator!=(const CodeWarning& that) const;

   //  Returns true if the log should be suppressed.
   //
   bool Suppress() const;

   //  Returns true if the log has code to display.
   //
   bool HasCodeToDisplay() const;

   //  Returns true if .info should be displayed.
   //
   bool HasInfoToDisplay() const;

   //  Returns true if the log is informational and cannot be fixed.
   //
   bool IsInformational() const;

   //  Returns the logs that need to be fixed to resolve this log.
   //  The log itself is included in the result unless it does not
   //  need to be fixed.
   //
   std::vector< CodeWarning* > LogsToFix(std::string& expl);

   //  Returns the other log associated with a warning that involves
   //  fixing both a declaration and a definition.
   //
   CodeWarning* FindMateLog(std::string& expl) const;

   //  Returns the name of the function that this warning wants added to
   //  a class.  Returns an empty string if LOG does not suggest this.
   //
   std::string GetNewFuncName() const;

   //  Updates WARNINGS with those that were logged in FILE.  Excludes
   //  informational warnings, which cannot be fixed.
   //
   static void GetWarnings
      (const CodeFile* file, std::vector< CodeWarning* >& warnings);

   //  Updates the position of a warning when a file has been edited.  Has
   //  the same interface as CxxToken::UpdatePos.
   //
   void UpdatePos(EditorAction action,
      size_t begin, size_t count, size_t from = std::string::npos) const;

   //  Returns true if LOG2 > LOG1 when sorting by file/line/reverse pos.
   //
   static bool IsSortedToFix(const CodeWarning* log1, const CodeWarning* log2);

   //  Returns LOG's index if it has already been reported, else -1.
   //
   static NodeBase::word FindWarning(const CodeWarning& log);

   //  Returns true if LOG2 > LOG1 when sorting by file/warning/line.
   //
   static bool IsSortedByFile(const CodeWarning& log1, const CodeWarning& log2);

   //  Returns true if LOG2 > LOG1 when sorting by warning/file/line.
   //
   static bool IsSortedByType(const CodeWarning& log1, const CodeWarning& log2);

   //  For inserting elements into the attributes map.
   //
   typedef std::pair< Warning, WarningAttrs > WarningPair;

   //  A type for mapping warnings to their attributes.
   //
   typedef std::map< Warning, WarningAttrs > AttrsMap;

   //  The type of warning.
   //
   Warning warning_;

   //  Where the warning occurred.
   //
   mutable CxxLocation loc_;

   //  The C++ item associated with the warning.
   //
   const CxxToken* item_;

   //  Warning specific.  If > 0, displayed after line number (used, for
   //  example, to specify that a function's Nth argument is associated
   //  with the warning).  If < 0, denotes an informational warning that
   //  cannot be fixed but that is associated with one that can.
   //
   word offset_;

   //  Warning specific.
   //
   std::string info_;

   //  Whether a warning can be, or has been, fixed by the Editor.
   //
   WarningStatus status_;

   //  Maps a warning to its attributes.
   //
   static AttrsMap Attrs_;

   //  Warnings found in all files.
   //
   static std::vector< CodeWarning > Warnings_;

   //  The total number of warnings of each type, globally.
   //
   static size_t WarningCounts_[Warning_N];

   //  The number of lines of each type, globally.
   //
   static size_t LineTypeCounts_[LineType_N];
};
}
#endif
