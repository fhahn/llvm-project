//===- BitstreamRemarkSerializer.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the implementation of the LLVM bitstream remark serializer
// using LLVM's bitstream writer.
//
//===----------------------------------------------------------------------===//

#include "llvm/Remarks/BitstreamRemarkSerializer.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Remarks/Remark.h"
#include <cassert>
#include <optional>

using namespace llvm;
using namespace llvm::remarks;

BitstreamRemarkSerializerHelper::BitstreamRemarkSerializerHelper(
    BitstreamRemarkContainerType ContainerType, raw_ostream &OS)
    : Bitstream(OS), ContainerType(ContainerType) {}

static void setRecordName(unsigned RecordID, BitstreamWriter &Bitstream,
                          SmallVectorImpl<uint64_t> &R, StringRef Str) {
  R.clear();
  R.push_back(RecordID);
  append_range(R, Str);
  Bitstream.EmitRecord(bitc::BLOCKINFO_CODE_SETRECORDNAME, R);
}

static void initBlock(unsigned BlockID, BitstreamWriter &Bitstream,
                      SmallVectorImpl<uint64_t> &R, StringRef Str) {
  R.clear();
  R.push_back(BlockID);
  Bitstream.EmitRecord(bitc::BLOCKINFO_CODE_SETBID, R);

  R.clear();
  append_range(R, Str);
  Bitstream.EmitRecord(bitc::BLOCKINFO_CODE_BLOCKNAME, R);
}

void BitstreamRemarkSerializerHelper::setupMetaBlockInfo() {
  // Setup the metadata block.
  initBlock(META_BLOCK_ID, Bitstream, R, MetaBlockName);

  // The container information.
  setRecordName(RECORD_META_CONTAINER_INFO, Bitstream, R,
                MetaContainerInfoName);

  auto Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_META_CONTAINER_INFO));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Version.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 2));  // Type.
  RecordMetaContainerInfoAbbrevID =
      Bitstream.EmitBlockInfoAbbrev(META_BLOCK_ID, Abbrev);
}

void BitstreamRemarkSerializerHelper::setupMetaRemarkVersion() {
  setRecordName(RECORD_META_REMARK_VERSION, Bitstream, R,
                MetaRemarkVersionName);

  auto Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_META_REMARK_VERSION));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Version.
  RecordMetaRemarkVersionAbbrevID =
      Bitstream.EmitBlockInfoAbbrev(META_BLOCK_ID, Abbrev);
}

void BitstreamRemarkSerializerHelper::emitMetaRemarkVersion(
    uint64_t RemarkVersion) {
  // The remark version is emitted only if we emit remarks.
  R.clear();
  R.push_back(RECORD_META_REMARK_VERSION);
  R.push_back(RemarkVersion);
  Bitstream.EmitRecordWithAbbrev(RecordMetaRemarkVersionAbbrevID, R);
}

void BitstreamRemarkSerializerHelper::setupMetaStrTab() {
  setRecordName(RECORD_META_STRTAB, Bitstream, R, MetaStrTabName);

  auto Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_META_STRTAB));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Raw table.
  RecordMetaStrTabAbbrevID =
      Bitstream.EmitBlockInfoAbbrev(META_BLOCK_ID, Abbrev);
}

void BitstreamRemarkSerializerHelper::emitMetaStrTab(
    const StringTable &StrTab) {
  // The string table is not emitted if we emit remarks separately.
  R.clear();
  R.push_back(RECORD_META_STRTAB);

  // Serialize to a blob.
  std::string Buf;
  raw_string_ostream OS(Buf);
  StrTab.serialize(OS);
  StringRef Blob = OS.str();
  Bitstream.EmitRecordWithBlob(RecordMetaStrTabAbbrevID, R, Blob);
}

void BitstreamRemarkSerializerHelper::setupMetaExternalFile() {
  setRecordName(RECORD_META_EXTERNAL_FILE, Bitstream, R, MetaExternalFileName);

  auto Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_META_EXTERNAL_FILE));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Filename.
  RecordMetaExternalFileAbbrevID =
      Bitstream.EmitBlockInfoAbbrev(META_BLOCK_ID, Abbrev);
}

void BitstreamRemarkSerializerHelper::emitMetaExternalFile(StringRef Filename) {
  // The external file is emitted only if we emit the separate metadata.
  R.clear();
  R.push_back(RECORD_META_EXTERNAL_FILE);
  Bitstream.EmitRecordWithBlob(RecordMetaExternalFileAbbrevID, R, Filename);
}

void BitstreamRemarkSerializerHelper::setupRemarkBlockInfo() {
  // Setup the remark block.
  initBlock(REMARKS_BLOCK_ID, Bitstream, R, RemarksBlockName);

  {
    setRecordName(RECORD_REMARK, Bitstream, R, RemarkName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 3)); // Type
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));   // Remark Name
    RecordRemarkAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }

  // The header of a remark.
  {
    setRecordName(RECORD_REMARK_HEADER, Bitstream, R, RemarkHeaderName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK_HEADER));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // Pass name
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8)); // Function name
    RecordRemarkHeaderAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }

  // The location of a remark.
  {
    setRecordName(RECORD_REMARK_DEBUG_LOC, Bitstream, R, RemarkDebugLocName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK_DEBUG_LOC));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 7)); // File
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // Line
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // Column
    RecordRemarkDebugLocAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }
  {
    setRecordName(RECORD_REMARK_DEBUG_LOC_FILE, Bitstream, R,
                  RemarkDebugLocFileName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK_DEBUG_LOC_FILE));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 7)); // File
    RecordRemarkDebugLocFileAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }

  // The hotness of a remark.
  {
    setRecordName(RECORD_REMARK_HOTNESS, Bitstream, R, RemarkHotnessName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK_HOTNESS));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8)); // Hotness
    RecordRemarkHotnessAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }

  // An argument entry with no debug location attached.
  {
    setRecordName(RECORD_REMARK_ARG_KV, Bitstream, R, RemarkArgKVName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK_ARG_KV));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 7)); // Key
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 7)); // Value
    RecordRemarkArgKVAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }

  {
    setRecordName(RECORD_REMARK_ARG_V, Bitstream, R, RemarkArgVName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK_ARG_V));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 7)); // Value
    RecordRemarkArgVAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }
  {
    setRecordName(RECORD_REMARK_ARG_KV_INT, Bitstream, R, RemarkArgKVIntName);

    auto Abbrev = std::make_shared<BitCodeAbbrev>();
    Abbrev->Add(BitCodeAbbrevOp(RECORD_REMARK_ARG_KV_INT));
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 7)); // Key
    Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 7)); // Value
    RecordRemarkArgKVIntAbbrevID =
        Bitstream.EmitBlockInfoAbbrev(REMARKS_BLOCK_ID, Abbrev);
  }
}

void BitstreamRemarkSerializerHelper::setupBlockInfo() {
  // Emit magic number.
  for (const char C : ContainerMagic)
    Bitstream.Emit(static_cast<unsigned>(C), 8);

  Bitstream.EnterBlockInfoBlock();
  auto ExitBlock = make_scope_exit([&] { Bitstream.ExitBlock(); });

  // Setup the main metadata. Depending on the container type, we'll setup the
  // required records next.
  setupMetaBlockInfo();

  switch (ContainerType) {
  case BitstreamRemarkContainerType::RemarksFileExternal:
    // Needs to know where the external remarks file is.
    setupMetaExternalFile();
    return;
  case BitstreamRemarkContainerType::RemarksFile:
    // Contains remarks: emit the version.
    setupMetaRemarkVersion();
    // Needs a string table.
    setupMetaStrTab();
    // Contains remarks: emit the remark abbrevs.
    setupRemarkBlockInfo();
    return;
  }
  llvm_unreachable("Unexpected BitstreamRemarkContainerType");
}

void BitstreamRemarkSerializerHelper::emitMetaBlock(
    std::optional<StringRef> Filename) {
  // Emit the meta block
  Bitstream.EnterSubblock(META_BLOCK_ID, 3);
  auto ExitBlock = make_scope_exit([&] { Bitstream.ExitBlock(); });

  // The container version and type.
  R.clear();
  R.push_back(RECORD_META_CONTAINER_INFO);
  R.push_back(CurrentContainerVersion);
  R.push_back(static_cast<uint64_t>(ContainerType));
  Bitstream.EmitRecordWithAbbrev(RecordMetaContainerInfoAbbrevID, R);

  switch (ContainerType) {
  case BitstreamRemarkContainerType::RemarksFileExternal:
    assert(Filename != std::nullopt);
    emitMetaExternalFile(*Filename);
    return;
  case BitstreamRemarkContainerType::RemarksFile:
    emitMetaRemarkVersion(CurrentRemarkVersion);
    return;
  }
  llvm_unreachable("Unexpected BitstreamRemarkContainerType");
}

void BitstreamRemarkSerializerHelper::enterRemarksBlock() {
  Bitstream.EnterSubblock(REMARKS_BLOCK_ID, 4);
}

void BitstreamRemarkSerializerHelper::exitRemarksBlock() {
  Bitstream.ExitBlock();
}

void BitstreamRemarkSerializerHelper::emitLateMetaBlock(
    const StringTable &StrTab) {
  // Emit the late meta block (after all remarks are serialized)
  Bitstream.EnterSubblock(META_BLOCK_ID, 3);
  emitMetaStrTab(StrTab);
  Bitstream.ExitBlock();
}

void BitstreamRemarkSerializerHelper::emitRemark(const Remark &Remark,
                                                      StringTable &StrTab) {
  if ((Bitstream.GetCurrentBlockBitNo() / 8) > (1 << 24)) {
    exitRemarksBlock();
    //FIXME:
    LastRemarkPass = StringRef{};
    LastRemarkFunction = StringRef{};
    enterRemarksBlock();
  }

  auto emitRemarkLoc = [&](const RemarkLocation &Loc) {
    R.clear();
    R.push_back(RECORD_REMARK_DEBUG_LOC);
    R.push_back(StrTab.add(Loc.SourceFilePath).first);
    R.push_back(Loc.SourceLine);
    R.push_back(Loc.SourceColumn);
    Bitstream.EmitRecordWithAbbrev(RecordRemarkDebugLocAbbrevID, R);
  };
    /*} else {*/
    /*  R.push_back(RECORD_REMARK_DEBUG_LOC_FILE);*/
    /*  R.push_back(StrTab.add(Loc.SourceFilePath).first);*/
    /*  Bitstream.EmitRecordWithAbbrev(RecordRemarkDebugLocFileAbbrevID, R);*/
    /*}*/

  if (LastRemarkPass != Remark.PassName ||
      LastRemarkFunction != Remark.FunctionName) {
    R.clear();
    R.push_back(RECORD_REMARK_HEADER);
    auto PassN = StrTab.add(Remark.PassName);
    auto FuncN = StrTab.add(Remark.FunctionName);
    R.push_back(PassN.first);
    R.push_back(FuncN.first);
    Bitstream.EmitRecordWithAbbrev(RecordRemarkHeaderAbbrevID, R);
    LastRemarkPass = PassN.second;
    LastRemarkFunction = FuncN.second;
  }

  R.clear();
  R.push_back(RECORD_REMARK);
  R.push_back(static_cast<uint64_t>(Remark.RemarkType));
  R.push_back(StrTab.add(Remark.RemarkName).first);
  Bitstream.EmitRecordWithAbbrev(RecordRemarkAbbrevID, R);

  if (const std::optional<RemarkLocation> &Loc = Remark.Loc) {
    emitRemarkLoc(*Loc);
  }

  if (std::optional<uint64_t> Hotness = Remark.Hotness) {
    R.clear();
    R.push_back(RECORD_REMARK_HOTNESS);
    R.push_back(*Hotness);
    Bitstream.EmitRecordWithAbbrev(RecordRemarkHotnessAbbrevID, R);
  }

  for (const Argument &Arg : Remark.Args) {
    R.clear();
    auto MaybeIntVal = Arg.getValAsInt();

    unsigned Opc = RECORD_REMARK_ARG_KV;
    if (Arg.Key == "String") {
      Opc = RECORD_REMARK_ARG_V;
    } else if (MaybeIntVal && *MaybeIntVal >= 0) {
      Opc = RECORD_REMARK_ARG_KV_INT;
    }
    R.push_back(Opc);

    if (Opc != RECORD_REMARK_ARG_V) {
      R.push_back(StrTab.add(Arg.Key).first);
    }
    if (Opc == RECORD_REMARK_ARG_KV_INT) {
      R.push_back(*MaybeIntVal);
    } else {
      R.push_back(StrTab.add(Arg.Val).first);
    }
    unsigned Abbrev;
    switch (Opc) {
    case RECORD_REMARK_ARG_KV:
      Abbrev = RecordRemarkArgKVAbbrevID;
      break;
    case RECORD_REMARK_ARG_KV_INT:
      Abbrev = RecordRemarkArgKVIntAbbrevID;
      break;
    case RECORD_REMARK_ARG_V:
      Abbrev = RecordRemarkArgVAbbrevID;
      break;
    default:
      llvm_unreachable("Illegal arg opc");
    }
    Bitstream.EmitRecordWithAbbrev(Abbrev, R);
    if (Arg.Loc) {
      emitRemarkLoc(*Arg.Loc);
    }
  }
}

BitstreamRemarkSerializer::BitstreamRemarkSerializer(raw_ostream &OS)
    : RemarkSerializer(Format::Bitstream, OS) {
  StrTab.emplace();
}

BitstreamRemarkSerializer::BitstreamRemarkSerializer(raw_ostream &OS,
                                                     StringTable StrTabIn)
    : RemarkSerializer(Format::Bitstream, OS) {
  StrTab = std::move(StrTabIn);
}

BitstreamRemarkSerializer::~BitstreamRemarkSerializer() { finalize(); }

void BitstreamRemarkSerializer::setup() {
  if (Helper)
    return;
  Helper.emplace(BitstreamRemarkContainerType::RemarksFile, OS);
  Helper->setupBlockInfo();
  Helper->emitMetaBlock();
}

void BitstreamRemarkSerializer::finalize() {
  if (!Helper)
    return;
  Helper->emitLateMetaBlock(*StrTab);
  Helper = std::nullopt;
}

void BitstreamRemarkSerializer::emit(const Remark &Remark) {
  setup();
  Helper->emitRemark(Remark, *StrTab);
}

std::unique_ptr<MetaSerializer>
BitstreamRemarkSerializer::metaSerializer(raw_ostream &OS,
                                          StringRef ExternalFilename) {
  return std::make_unique<BitstreamMetaSerializer>(
      OS, BitstreamRemarkContainerType::RemarksFileExternal, ExternalFilename);
}

void BitstreamMetaSerializer::emit() {
  assert(Helper && "BitstreamMetaSerializer emitted multiple times");
  Helper->setupBlockInfo();
  Helper->emitMetaBlock(ExternalFilename);
  Helper = std::nullopt;
}
