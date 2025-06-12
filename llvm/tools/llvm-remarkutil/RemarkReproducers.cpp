//===- RemarkReproducers.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Generic tool to extract IR reproducers from remarks
//
//===----------------------------------------------------------------------===//

#include "RemarkUtilHelpers.h"
#include "RemarkUtilRegistry.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Regex.h"

using namespace llvm;
using namespace remarks;
using namespace llvm::remarkutil;

namespace instructionmix {

static cl::SubCommand Reproducers("reproducers", "Extract IR reproducers");

INPUT_FORMAT_COMMAND_LINE_OPTIONS(Reproducers)
INPUT_OUTPUT_COMMAND_LINE_OPTIONS(Reproducers)

static Error tryReproducers() {
  auto MaybeOF = getOutputFileWithFlags(OutputFileName, sys::fs::OF_Text);
  if (!MaybeOF)
    return MaybeOF.takeError();

  auto OF = std::move(*MaybeOF);
  auto MaybeBuf = getInputMemoryBuffer(InputFileName);
  if (!MaybeBuf)
    return MaybeBuf.takeError();
  auto MaybeParser = createRemarkParser(InputFormat, (*MaybeBuf)->getBuffer());
  if (!MaybeParser)
    return MaybeParser.takeError();

  auto &Parser = **MaybeParser;
  auto MaybeRemark = Parser.next();
  llvm::DenseMap<StringRef, size_t> Stats;
  llvm::DenseMap<StringRef, size_t> ModuleIdx;
  for (; MaybeRemark; MaybeRemark = Parser.next()) {
    Remark &Remark = **MaybeRemark;
    if (Remark.RemarkName == "FuncStats") {
      for (auto &Arg : Remark.Args) {
        auto Val = Arg.getValAsInt();
        if (!Val)
          return createStringError("Illegal FuncStats");
        Stats[Arg.Key] += *Val;
      }
    }
    if (!Remark.Blob)
      continue;
    auto Name = Remark.RemarkName;
    auto NameIdx = ModuleIdx[Name]++;
    StringRef Ext = ".txt";
    if (Remark.Tags.contains(remarks::Tag::BitCodeBlob)) {
      Ext = ".bc";
    } else if (Remark.Tags.contains(remarks::Tag::IRBlob)) {
      Ext = ".ll";
    } else if (Remark.Tags.contains(remarks::Tag::GenericBinaryBlob)) {
      Ext = ".blob";
    }
    std::error_code ErrorCode;
    std::string DumpFile =
        (Twine(Name) + (NameIdx != 0 ? "." + Twine(itostr(NameIdx)) : Twine()) +
         Ext)
            .str();
    auto OF =
        std::make_unique<ToolOutputFile>(DumpFile, ErrorCode, sys::fs::OF_Text);
    if (ErrorCode)
      return errorCodeToError(ErrorCode);

    OF->os() << *Remark.Blob;
    OF->keep();
  }
  for (auto &Stat : Stats) {
    OF->os() << Stat.getFirst() << ": " << Stat.getSecond() << "\n";
  }

  auto E = MaybeRemark.takeError();
  if (!E.isA<EndOfFileError>())
    return E;
  consumeError(std::move(E));
  return Error::success();
}

static CommandRegistration ReproducerReg(&Reproducers, tryReproducers);

} // namespace instructionmix
