//===- PGOCtxProfReader.cpp - Contextual Instrumentation profile reader ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Read a contextual profile into a datastructure suitable for maintenance
// throughout IPO
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/PGOCtxProfReader.h"
#include "llvm/Bitstream/BitCodeEnums.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/PGOCtxProfWriter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/YAMLTraits.h"
#include <utility>

using namespace llvm;

// FIXME(#92054) - these Error handling macros are (re-)invented in a few
// places.
#define EXPECT_OR_RET(LHS, RHS)                                                \
  auto LHS = RHS;                                                              \
  if (!LHS)                                                                    \
    return LHS.takeError();

#define RET_ON_ERR(EXPR)                                                       \
  if (auto Err = (EXPR))                                                       \
    return Err;

Expected<PGOCtxProfContext &>
PGOCtxProfContext::getOrEmplace(uint32_t Index, GlobalValue::GUID G,
                                SmallVectorImpl<uint64_t> &&Counters) {
  auto [Iter, Inserted] =
      Callsites[Index].insert({G, PGOCtxProfContext(G, std::move(Counters))});
  if (!Inserted)
    return make_error<InstrProfError>(instrprof_error::invalid_prof,
                                      "Duplicate GUID for same callsite.");
  return Iter->second;
}

Expected<BitstreamEntry> PGOCtxProfileReader::advance() {
  return Cursor.advance(BitstreamCursor::AF_DontAutoprocessAbbrevs);
}

Error PGOCtxProfileReader::wrongValue(const Twine &Msg) {
  return make_error<InstrProfError>(instrprof_error::invalid_prof, Msg);
}

Error PGOCtxProfileReader::unsupported(const Twine &Msg) {
  return make_error<InstrProfError>(instrprof_error::unsupported_version, Msg);
}

bool PGOCtxProfileReader::tryGetNextKnownBlockID(PGOCtxProfileBlockIDs &ID) {
  auto Blk = advance();
  if (!Blk) {
    consumeError(Blk.takeError());
    return false;
  }
  if (Blk->Kind != BitstreamEntry::SubBlock)
    return false;
  if (PGOCtxProfileBlockIDs::FIRST_VALID > Blk->ID ||
      PGOCtxProfileBlockIDs::LAST_VALID < Blk->ID)
    return false;
  ID = static_cast<PGOCtxProfileBlockIDs>(Blk->ID);
  return true;
}

bool PGOCtxProfileReader::canEnterBlockWithID(PGOCtxProfileBlockIDs ID) {
  PGOCtxProfileBlockIDs Test = {};
  return tryGetNextKnownBlockID(Test) && Test == ID;
}

Error PGOCtxProfileReader::enterBlockWithID(PGOCtxProfileBlockIDs ID) {
  RET_ON_ERR(Cursor.EnterSubBlock(ID));
  return Error::success();
}

// Note: we use PGOCtxProfContext for flat profiles also, as the latter are
// structurally similar. Alternative modeling here seems a bit overkill at the
// moment.
Expected<std::pair<std::optional<uint32_t>, PGOCtxProfContext>>
PGOCtxProfileReader::readProfile(PGOCtxProfileBlockIDs Kind) {
  assert((Kind == PGOCtxProfileBlockIDs::ContextRootBlockID ||
          Kind == PGOCtxProfileBlockIDs::ContextNodeBlockID ||
          Kind == PGOCtxProfileBlockIDs::FlatProfileBlockID) &&
         "Unexpected profile kind");
  RET_ON_ERR(enterBlockWithID(Kind));

  std::optional<ctx_profile::GUID> Guid;
  std::optional<SmallVector<uint64_t, 16>> Counters;
  std::optional<uint32_t> CallsiteIndex;
  std::optional<uint64_t> TotalEntryCount;
  std::optional<CtxProfFlatProfile> Unhandled;
  SmallVector<uint64_t, 1> RecordValues;

  const bool ExpectIndex = Kind == PGOCtxProfileBlockIDs::ContextNodeBlockID;
  const bool IsRoot = Kind == PGOCtxProfileBlockIDs::ContextRootBlockID;
  // We don't prescribe the order in which the records come in, and we are ok
  // if other unsupported records appear. We seek in the current subblock until
  // we get all we know.
  auto GotAllWeNeed = [&]() {
    return Guid.has_value() && Counters.has_value() &&
           (!ExpectIndex || CallsiteIndex.has_value()) &&
           (!IsRoot || TotalEntryCount.has_value()) &&
           (!IsRoot || Unhandled.has_value());
  };

  while (!GotAllWeNeed()) {
    RecordValues.clear();
    EXPECT_OR_RET(Entry, advance());
    if (Entry->Kind != BitstreamEntry::Record) {
      if (IsRoot && Entry->Kind == BitstreamEntry::SubBlock &&
          Entry->ID == PGOCtxProfileBlockIDs::UnhandledBlockID) {
        RET_ON_ERR(enterBlockWithID(PGOCtxProfileBlockIDs::UnhandledBlockID));
        Unhandled = CtxProfFlatProfile();
        RET_ON_ERR(loadFlatProfileList(*Unhandled));
        continue;
      }
      return wrongValue(
          "Expected records before encountering more subcontexts");
    }
    EXPECT_OR_RET(ReadRecord,
                  Cursor.readRecord(bitc::UNABBREV_RECORD, RecordValues));
    switch (*ReadRecord) {
    case PGOCtxProfileRecords::Guid:
      if (RecordValues.size() != 1)
        return wrongValue("The GUID record should have exactly one value");
      Guid = RecordValues[0];
      break;
    case PGOCtxProfileRecords::Counters:
      Counters = std::move(RecordValues);
      if (Counters->empty())
        return wrongValue("Empty counters. At least the entry counter (one "
                          "value) was expected");
      break;
    case PGOCtxProfileRecords::CallsiteIndex:
      if (!ExpectIndex)
        return wrongValue("The root context should not have a callee index");
      if (RecordValues.size() != 1)
        return wrongValue("The callee index should have exactly one value");
      CallsiteIndex = RecordValues[0];
      break;
    case PGOCtxProfileRecords::TotalRootEntryCount:
      if (!IsRoot)
        return wrongValue("Non-root has a total entry count record");
      if (RecordValues.size() != 1)
        return wrongValue(
            "The root total entry count record should have exactly one value");
      TotalEntryCount = RecordValues[0];
      break;
    default:
      // OK if we see records we do not understand, like records (profile
      // components) introduced later.
      break;
    }
  }

  PGOCtxProfContext Ret(*Guid, std::move(*Counters), TotalEntryCount,
                        std::move(Unhandled));

  while (canEnterBlockWithID(PGOCtxProfileBlockIDs::ContextNodeBlockID)) {
    EXPECT_OR_RET(SC, readProfile(PGOCtxProfileBlockIDs::ContextNodeBlockID));
    auto &Targets = Ret.callsites()[*SC->first];
    auto [_, Inserted] =
        Targets.insert({SC->second.guid(), std::move(SC->second)});
    if (!Inserted)
      return wrongValue(
          "Unexpected duplicate target (callee) at the same callsite.");
  }
  return std::make_pair(CallsiteIndex, std::move(Ret));
}

Error PGOCtxProfileReader::readMetadata() {
  if (Magic.size() < PGOCtxProfileWriter::ContainerMagic.size() ||
      Magic != PGOCtxProfileWriter::ContainerMagic)
    return make_error<InstrProfError>(instrprof_error::invalid_prof,
                                      "Invalid magic");

  BitstreamEntry Entry;
  RET_ON_ERR(Cursor.advance().moveInto(Entry));
  if (Entry.Kind != BitstreamEntry::SubBlock ||
      Entry.ID != bitc::BLOCKINFO_BLOCK_ID)
    return unsupported("Expected Block ID");
  // We don't need the blockinfo to read the rest, it's metadata usable for e.g.
  // llvm-bcanalyzer.
  RET_ON_ERR(Cursor.SkipBlock());

  EXPECT_OR_RET(Blk, advance());
  if (Blk->Kind != BitstreamEntry::SubBlock)
    return unsupported("Expected Version record");
  RET_ON_ERR(
      Cursor.EnterSubBlock(PGOCtxProfileBlockIDs::ProfileMetadataBlockID));
  EXPECT_OR_RET(MData, advance());
  if (MData->Kind != BitstreamEntry::Record)
    return unsupported("Expected Version record");

  SmallVector<uint64_t, 1> Ver;
  EXPECT_OR_RET(Code, Cursor.readRecord(bitc::UNABBREV_RECORD, Ver));
  if (*Code != PGOCtxProfileRecords::Version)
    return unsupported("Expected Version record");
  if (Ver.size() != 1 || Ver[0] > PGOCtxProfileWriter::CurrentVersion)
    return unsupported("Version " + Twine(*Code) +
                       " is higher than supported version " +
                       Twine(PGOCtxProfileWriter::CurrentVersion));
  return Error::success();
}

Error PGOCtxProfileReader::loadContexts(CtxProfContextualProfiles &P) {
  RET_ON_ERR(enterBlockWithID(PGOCtxProfileBlockIDs::ContextsSectionBlockID));
  while (canEnterBlockWithID(PGOCtxProfileBlockIDs::ContextRootBlockID)) {
    EXPECT_OR_RET(E, readProfile(PGOCtxProfileBlockIDs::ContextRootBlockID));
    auto Key = E->second.guid();
    if (!P.insert({Key, std::move(E->second)}).second)
      return wrongValue("Duplicate roots");
  }
  return Error::success();
}

Error PGOCtxProfileReader::loadFlatProfileList(CtxProfFlatProfile &P) {
  while (canEnterBlockWithID(PGOCtxProfileBlockIDs::FlatProfileBlockID)) {
    EXPECT_OR_RET(E, readProfile(PGOCtxProfileBlockIDs::FlatProfileBlockID));
    auto Guid = E->second.guid();
    if (!P.insert({Guid, std::move(E->second.counters())}).second)
      return wrongValue("Duplicate flat profile entries");
  }
  return Error::success();
}

Error PGOCtxProfileReader::loadFlatProfiles(CtxProfFlatProfile &P) {
  RET_ON_ERR(
      enterBlockWithID(PGOCtxProfileBlockIDs::FlatProfilesSectionBlockID));
  return loadFlatProfileList(P);
}

Expected<PGOCtxProfile> PGOCtxProfileReader::loadProfiles() {
  RET_ON_ERR(readMetadata());
  PGOCtxProfile Ret;
  PGOCtxProfileBlockIDs Test = {};
  for (auto I = 0; I < 2; ++I) {
    if (!tryGetNextKnownBlockID(Test))
      break;
    if (Test == PGOCtxProfileBlockIDs::ContextsSectionBlockID) {
      RET_ON_ERR(loadContexts(Ret.Contexts));
    } else if (Test == PGOCtxProfileBlockIDs::FlatProfilesSectionBlockID) {
      RET_ON_ERR(loadFlatProfiles(Ret.FlatProfiles));
    } else {
      return wrongValue("Unexpected section");
    }
  }

  return std::move(Ret);
}

namespace {
// We want to pass `const` values PGOCtxProfContext references to the yaml
// converter, and the regular yaml mapping APIs are designed to handle both
// serialization and deserialization, which prevents using const for
// serialization. Using an intermediate datastructure is overkill, both
// space-wise and design complexity-wise. Instead, we use the lower-level APIs.
void toYaml(yaml::Output &Out, const PGOCtxProfContext &Ctx);

void toYaml(yaml::Output &Out,
            const PGOCtxProfContext::CallTargetMapTy &CallTargets) {
  Out.beginSequence();
  size_t Index = 0;
  void *SaveData = nullptr;
  for (const auto &[_, Ctx] : CallTargets) {
    Out.preflightElement(Index++, SaveData);
    toYaml(Out, Ctx);
    Out.postflightElement(nullptr);
  }
  Out.endSequence();
}

void toYaml(yaml::Output &Out,
            const PGOCtxProfContext::CallsiteMapTy &Callsites) {
  auto AllCS = ::llvm::make_first_range(Callsites);
  auto MaxIt = ::llvm::max_element(AllCS);
  assert(MaxIt != AllCS.end() && "We should have a max value because the "
                                 "callsites collection is not empty.");
  void *SaveData = nullptr;
  Out.beginSequence();
  for (auto I = 0U; I <= *MaxIt; ++I) {
    Out.preflightElement(I, SaveData);
    auto It = Callsites.find(I);
    if (It == Callsites.end()) {
      // This will produce a `[ ]` sequence, which is what we want here.
      Out.beginFlowSequence();
      Out.endFlowSequence();
    } else {
      toYaml(Out, It->second);
    }
    Out.postflightElement(nullptr);
  }
  Out.endSequence();
}

void toYaml(yaml::Output &Out, const CtxProfFlatProfile &Flat);

void toYaml(yaml::Output &Out, GlobalValue::GUID Guid,
            const SmallVectorImpl<uint64_t> &Counters,
            const PGOCtxProfContext::CallsiteMapTy &Callsites,
            std::optional<uint64_t> TotalRootEntryCount = std::nullopt,
            CtxProfFlatProfile Unhandled = {}) {
  yaml::EmptyContext Empty;
  Out.beginMapping();
  void *SaveInfo = nullptr;
  bool UseDefault = false;
  {
    Out.preflightKey("Guid", /*Required=*/true, /*SameAsDefault=*/false,
                     UseDefault, SaveInfo);
    yaml::yamlize(Out, Guid, true, Empty);
    Out.postflightKey(nullptr);
  }
  if (TotalRootEntryCount) {
    Out.preflightKey("TotalRootEntryCount", true, false, UseDefault, SaveInfo);
    yaml::yamlize(Out, *TotalRootEntryCount, true, Empty);
    Out.postflightKey(nullptr);
  }
  {
    Out.preflightKey("Counters", true, false, UseDefault, SaveInfo);
    Out.beginFlowSequence();
    for (size_t I = 0U, E = Counters.size(); I < E; ++I) {
      Out.preflightFlowElement(I, SaveInfo);
      uint64_t V = Counters[I];
      yaml::yamlize(Out, V, true, Empty);
      Out.postflightFlowElement(SaveInfo);
    }
    Out.endFlowSequence();
    Out.postflightKey(nullptr);
  }

  if (!Unhandled.empty()) {
    assert(TotalRootEntryCount.has_value());
    Out.preflightKey("Unhandled", false, false, UseDefault, SaveInfo);
    toYaml(Out, Unhandled);
    Out.postflightKey(nullptr);
  }

  if (!Callsites.empty()) {
    Out.preflightKey("Callsites", true, false, UseDefault, SaveInfo);
    toYaml(Out, Callsites);
    Out.postflightKey(nullptr);
  }
  Out.endMapping();
}

void toYaml(yaml::Output &Out, const CtxProfFlatProfile &Flat) {
  void *SaveInfo = nullptr;
  Out.beginSequence();
  size_t ElemID = 0;
  for (const auto &[Guid, Counters] : Flat) {
    Out.preflightElement(ElemID++, SaveInfo);
    toYaml(Out, Guid, Counters, {});
    Out.postflightElement(nullptr);
  }
  Out.endSequence();
}

void toYaml(yaml::Output &Out, const PGOCtxProfContext &Ctx) {
  if (Ctx.isRoot())
    toYaml(Out, Ctx.guid(), Ctx.counters(), Ctx.callsites(),
           Ctx.getTotalRootEntryCount(), Ctx.getUnhandled());
  else
    toYaml(Out, Ctx.guid(), Ctx.counters(), Ctx.callsites());
}

} // namespace

void llvm::convertCtxProfToYaml(raw_ostream &OS, const PGOCtxProfile &Profile) {
  yaml::Output Out(OS);
  void *SaveInfo = nullptr;
  bool UseDefault = false;
  Out.beginMapping();
  if (!Profile.Contexts.empty()) {
    Out.preflightKey("Contexts", false, false, UseDefault, SaveInfo);
    toYaml(Out, Profile.Contexts);
    Out.postflightKey(nullptr);
  }
  if (!Profile.FlatProfiles.empty()) {
    Out.preflightKey("FlatProfiles", false, false, UseDefault, SaveInfo);
    toYaml(Out, Profile.FlatProfiles);
    Out.postflightKey(nullptr);
  }
  Out.endMapping();
}
