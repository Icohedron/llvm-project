//===-- lib/Semantics/compute-offsets.cpp -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "compute-offsets.h"
#include "flang/Evaluate/fold-designator.h"
#include "flang/Evaluate/fold.h"
#include "flang/Evaluate/shape.h"
#include "flang/Evaluate/type.h"
#include "flang/Runtime/descriptor-consts.h"
#include "flang/Semantics/scope.h"
#include "flang/Semantics/semantics.h"
#include "flang/Semantics/symbol.h"
#include "flang/Semantics/tools.h"
#include "flang/Semantics/type.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <vector>

namespace Fortran::semantics {

class ComputeOffsetsHelper {
public:
  ComputeOffsetsHelper(SemanticsContext &context) : context_{context} {}
  void Compute(Scope &);

private:
  struct SizeAndAlignment {
    SizeAndAlignment() {}
    SizeAndAlignment(std::size_t bytes) : size{bytes}, alignment{bytes} {}
    SizeAndAlignment(std::size_t bytes, std::size_t align)
        : size{bytes}, alignment{align} {}
    std::size_t size{0};
    std::size_t alignment{0};
  };
  struct SymbolAndOffset {
    SymbolAndOffset(Symbol &s, std::size_t off, const EquivalenceObject &obj)
        : symbol{s}, offset{off}, object{&obj} {}
    SymbolAndOffset(const SymbolAndOffset &) = default;
    MutableSymbolRef symbol;
    std::size_t offset;
    const EquivalenceObject *object;
  };

  void DoCommonBlock(Symbol &);
  void DoEquivalenceBlockBase(Symbol &, SizeAndAlignment &);
  void DoEquivalenceSet(const EquivalenceSet &);
  SymbolAndOffset Resolve(const SymbolAndOffset &);
  std::size_t ComputeOffset(const EquivalenceObject &);
  // Returns amount of padding that was needed for alignment
  std::size_t DoSymbol(
      Symbol &, std::optional<const size_t> newAlign = std::nullopt);
  SizeAndAlignment GetSizeAndAlignment(const Symbol &, bool entire);
  std::size_t Align(std::size_t, std::size_t);
  std::optional<size_t> CompAlignment(const Symbol &);
  std::optional<size_t> HasSpecialAlign(const Symbol &, Scope &);

  SemanticsContext &context_;
  std::size_t offset_{0};
  std::size_t alignment_{1};
  // symbol -> symbol+offset that determines its location, from EQUIVALENCE
  std::map<MutableSymbolRef, SymbolAndOffset, SymbolAddressCompare> dependents_;
  // base symbol -> SizeAndAlignment for each distinct EQUIVALENCE block
  std::map<MutableSymbolRef, SizeAndAlignment, SymbolAddressCompare>
      equivalenceBlock_;
};

// This function is only called if the target platform is AIX.
static bool isReal8OrLarger(const Fortran::semantics::DeclTypeSpec *type) {
  return ((type->IsNumeric(common::TypeCategory::Real) ||
              type->IsNumeric(common::TypeCategory::Complex)) &&
      evaluate::ToInt64(type->numericTypeSpec().kind()) > 4);
}

// This function is only called if the target platform is AIX.
// It determines the alignment of a component. If the component is a derived
// type, the alignment is computed accordingly.
std::optional<size_t> ComputeOffsetsHelper::CompAlignment(const Symbol &sym) {
  size_t max_align{0};
  constexpr size_t fourByteAlign{4};
  bool contain_double{false};
  auto derivedTypeSpec{sym.GetType()->AsDerived()};
  DirectComponentIterator directs{*derivedTypeSpec};
  for (auto it{directs.begin()}; it != directs.end(); ++it) {
    auto type{it->GetType()};
    auto s{GetSizeAndAlignment(*it, true)};
    if (isReal8OrLarger(type)) {
      max_align = std::max(max_align, fourByteAlign);
      contain_double = true;
    } else if (type->AsDerived()) {
      if (const auto newAlgin{CompAlignment(*it)}) {
        max_align = std::max(max_align, s.alignment);
      } else {
        return std::nullopt;
      }
    } else {
      max_align = std::max(max_align, s.alignment);
    }
  }

  if (contain_double) {
    return max_align;
  } else {
    return std::nullopt;
  }
}

// This function is only called if the target platform is AIX.
// Special alignment is needed only if it is a bind(c) derived type
// and contain real type components that have larger than 4 bytes.
std::optional<size_t> ComputeOffsetsHelper::HasSpecialAlign(
    const Symbol &sym, Scope &scope) {
  // On AIX, if the component that is not the first component and is
  // a float of 8 bytes or larger, it has the 4-byte alignment.
  // Only set the special alignment for bind(c) derived type on that platform.
  if (const auto type{sym.GetType()}) {
    auto &symOwner{sym.owner()};
    if (symOwner.symbol() && symOwner.IsDerivedType() &&
        symOwner.symbol()->attrs().HasAny({semantics::Attr::BIND_C}) &&
        &sym != &(*scope.GetSymbols().front())) {
      if (isReal8OrLarger(type)) {
        return 4UL;
      } else if (type->AsDerived()) {
        return CompAlignment(sym);
      }
    }
  }
  return std::nullopt;
}

void ComputeOffsetsHelper::Compute(Scope &scope) {
  for (Scope &child : scope.children()) {
    ComputeOffsets(context_, child);
  }
  if (scope.symbol() && scope.IsDerivedTypeWithKindParameter()) {
    return; // only process instantiations of kind parameterized derived types
  }
  if (scope.alignment().has_value()) {
    return; // prevent infinite recursion in error cases
  }
  scope.SetAlignment(0);
  // Build dependents_ from equivalences: symbol -> symbol+offset
  for (const EquivalenceSet &set : scope.equivalenceSets()) {
    DoEquivalenceSet(set);
  }
  // Compute a base symbol and overall block size for each
  // disjoint EQUIVALENCE storage sequence.
  for (auto &[symbol, dep] : dependents_) {
    dep = Resolve(dep);
    CHECK(symbol->size() == 0);
    auto symInfo{GetSizeAndAlignment(*symbol, true)};
    symbol->set_size(symInfo.size);
    Symbol &base{*dep.symbol};
    auto iter{equivalenceBlock_.find(base)};
    std::size_t minBlockSize{dep.offset + symInfo.size};
    if (iter == equivalenceBlock_.end()) {
      equivalenceBlock_.emplace(
          base, SizeAndAlignment{minBlockSize, symInfo.alignment});
    } else {
      SizeAndAlignment &blockInfo{iter->second};
      blockInfo.size = std::max(blockInfo.size, minBlockSize);
      blockInfo.alignment = std::max(blockInfo.alignment, symInfo.alignment);
    }
  }
  // Assign offsets for non-COMMON EQUIVALENCE blocks
  for (auto &[symbol, blockInfo] : equivalenceBlock_) {
    if (!FindCommonBlockContaining(*symbol)) {
      DoSymbol(*symbol);
      DoEquivalenceBlockBase(*symbol, blockInfo);
      offset_ = std::max(offset_, symbol->offset() + blockInfo.size);
    }
  }
  // Process remaining non-COMMON symbols; this is all of them if there
  // was no use of EQUIVALENCE in the scope.
  for (auto &symbol : scope.GetSymbols()) {
    if (!FindCommonBlockContaining(*symbol) &&
        dependents_.find(symbol) == dependents_.end() &&
        equivalenceBlock_.find(symbol) == equivalenceBlock_.end()) {

      std::optional<size_t> newAlign{std::nullopt};
      // Handle special alignment requirement for AIX
      auto triple{llvm::Triple(
          llvm::Triple::normalize(llvm::sys::getDefaultTargetTriple()))};
      if (triple.getOS() == llvm::Triple::OSType::AIX) {
        newAlign = HasSpecialAlign(*symbol, scope);
      }
      DoSymbol(*symbol, newAlign);
      if (auto *generic{symbol->detailsIf<GenericDetails>()}) {
        if (Symbol * specific{generic->specific()};
            specific && !FindCommonBlockContaining(*specific)) {
          // might be a shadowed procedure pointer
          DoSymbol(*specific);
        }
      }
    }
  }
  // Ensure that the size is a multiple of the alignment
  offset_ = Align(offset_, alignment_);
  scope.set_size(offset_);
  scope.SetAlignment(alignment_);
  // Assign offsets in COMMON blocks, unless this scope is a BLOCK construct,
  // where COMMON blocks are illegal (C1107 and C1108).
  if (scope.kind() != Scope::Kind::BlockConstruct) {
    for (auto &pair : scope.commonBlocks()) {
      DoCommonBlock(*pair.second);
    }
  }
  for (auto &[symbol, dep] : dependents_) {
    symbol->set_offset(dep.symbol->offset() + dep.offset);
    if (const auto *block{FindCommonBlockContaining(*dep.symbol)}) {
      symbol->get<ObjectEntityDetails>().set_commonBlock(*block);
    }
  }
}

auto ComputeOffsetsHelper::Resolve(const SymbolAndOffset &dep)
    -> SymbolAndOffset {
  auto it{dependents_.find(*dep.symbol)};
  if (it == dependents_.end()) {
    return dep;
  } else {
    SymbolAndOffset result{Resolve(it->second)};
    result.offset += dep.offset;
    result.object = dep.object;
    return result;
  }
}

void ComputeOffsetsHelper::DoCommonBlock(Symbol &commonBlock) {
  auto &details{commonBlock.get<CommonBlockDetails>()};
  offset_ = 0;
  alignment_ = 0;
  std::size_t minSize{0};
  std::size_t minAlignment{0};
  UnorderedSymbolSet previous;
  for (auto object : details.objects()) {
    Symbol &symbol{*object};
    auto errorSite{
        commonBlock.name().empty() ? symbol.name() : commonBlock.name()};
    if (std::size_t padding{DoSymbol(symbol.GetUltimate())}) {
      context_.Warn(common::UsageWarning::CommonBlockPadding, errorSite,
          "COMMON block /%s/ requires %zd bytes of padding before '%s' for alignment"_port_en_US,
          commonBlock.name(), padding, symbol.name());
    }
    previous.emplace(symbol);
    auto eqIter{equivalenceBlock_.end()};
    auto iter{dependents_.find(symbol)};
    if (iter == dependents_.end()) {
      eqIter = equivalenceBlock_.find(symbol);
      if (eqIter != equivalenceBlock_.end()) {
        DoEquivalenceBlockBase(symbol, eqIter->second);
      }
    } else {
      SymbolAndOffset &dep{iter->second};
      Symbol &base{*dep.symbol};
      if (const auto *baseBlock{FindCommonBlockContaining(base)}) {
        if (baseBlock == &commonBlock) {
          if (previous.find(SymbolRef{base}) == previous.end() ||
              base.offset() != symbol.offset() - dep.offset) {
            context_.Say(errorSite,
                "'%s' is storage associated with '%s' by EQUIVALENCE elsewhere in COMMON block /%s/"_err_en_US,
                symbol.name(), base.name(), commonBlock.name());
          }
        } else { // F'2023 8.10.3 p1
          context_.Say(errorSite,
              "'%s' in COMMON block /%s/ must not be storage associated with '%s' in COMMON block /%s/ by EQUIVALENCE"_err_en_US,
              symbol.name(), commonBlock.name(), base.name(),
              baseBlock->name());
        }
      } else if (dep.offset > symbol.offset()) { // 8.10.3(3)
        context_.Say(errorSite,
            "'%s' cannot backward-extend COMMON block /%s/ via EQUIVALENCE with '%s'"_err_en_US,
            symbol.name(), commonBlock.name(), base.name());
      } else {
        eqIter = equivalenceBlock_.find(base);
        base.get<ObjectEntityDetails>().set_commonBlock(commonBlock);
        base.set_offset(symbol.offset() - dep.offset);
        previous.emplace(base);
      }
    }
    // Get full extent of any EQUIVALENCE block into size of COMMON ( see
    // 8.10.2.2 point 1 (2))
    if (eqIter != equivalenceBlock_.end()) {
      SizeAndAlignment &blockInfo{eqIter->second};
      minSize = std::max(
          minSize, std::max(offset_, eqIter->first->offset() + blockInfo.size));
      minAlignment = std::max(minAlignment, blockInfo.alignment);
    }
  }
  commonBlock.set_size(std::max(minSize, offset_));
  details.set_alignment(std::max(minAlignment, alignment_));
  context_.MapCommonBlockAndCheckConflicts(commonBlock);
}

void ComputeOffsetsHelper::DoEquivalenceBlockBase(
    Symbol &symbol, SizeAndAlignment &blockInfo) {
  if (symbol.size() > blockInfo.size) {
    blockInfo.size = symbol.size();
  }
}

void ComputeOffsetsHelper::DoEquivalenceSet(const EquivalenceSet &set) {
  std::vector<SymbolAndOffset> symbolOffsets;
  std::optional<std::size_t> representative;
  for (const EquivalenceObject &object : set) {
    std::size_t offset{ComputeOffset(object)};
    SymbolAndOffset resolved{
        Resolve(SymbolAndOffset{object.symbol, offset, object})};
    symbolOffsets.push_back(resolved);
    if (!representative ||
        resolved.offset >= symbolOffsets[*representative].offset) {
      // The equivalenced object with the largest offset from its resolved
      // symbol will be the representative of this set, since the offsets
      // of the other objects will be positive relative to it.
      representative = symbolOffsets.size() - 1;
    }
  }
  CHECK(representative);
  const SymbolAndOffset &base{symbolOffsets[*representative]};
  for (const auto &[symbol, offset, object] : symbolOffsets) {
    if (symbol == base.symbol) {
      if (offset != base.offset) {
        auto x{evaluate::OffsetToDesignator(
            context_.foldingContext(), *symbol, base.offset, 1)};
        auto y{evaluate::OffsetToDesignator(
            context_.foldingContext(), *symbol, offset, 1)};
        if (x && y) {
          context_
              .Say(base.object->source,
                  "'%s' and '%s' cannot have the same first storage unit"_err_en_US,
                  x->AsFortran(), y->AsFortran())
              .Attach(object->source, "Incompatible reference to '%s'"_en_US,
                  y->AsFortran());
        } else { // error recovery
          context_
              .Say(base.object->source,
                  "'%s' (offset %zd bytes and %zd bytes) cannot have the same first storage unit"_err_en_US,
                  symbol->name(), base.offset, offset)
              .Attach(object->source,
                  "Incompatible reference to '%s' offset %zd bytes"_en_US,
                  symbol->name(), offset);
        }
      }
    } else {
      dependents_.emplace(*symbol,
          SymbolAndOffset{*base.symbol, base.offset - offset, *object});
    }
  }
}

// Offset of this equivalence object from the start of its variable.
std::size_t ComputeOffsetsHelper::ComputeOffset(
    const EquivalenceObject &object) {
  std::size_t offset{0};
  if (!object.subscripts.empty()) {
    if (const auto *details{object.symbol.detailsIf<ObjectEntityDetails>()}) {
      const ArraySpec &shape{details->shape()};
      auto lbound{[&](std::size_t i) {
        return *ToInt64(shape[i].lbound().GetExplicit());
      }};
      auto ubound{[&](std::size_t i) {
        return *ToInt64(shape[i].ubound().GetExplicit());
      }};
      for (std::size_t i{object.subscripts.size() - 1};;) {
        offset += object.subscripts[i] - lbound(i);
        if (i == 0) {
          break;
        }
        --i;
        offset *= ubound(i) - lbound(i) + 1;
      }
    }
  }
  auto result{offset * GetSizeAndAlignment(object.symbol, false).size};
  if (object.substringStart) {
    int kind{context_.defaultKinds().GetDefaultKind(TypeCategory::Character)};
    if (const DeclTypeSpec * type{object.symbol.GetType()}) {
      if (const IntrinsicTypeSpec * intrinsic{type->AsIntrinsic()}) {
        kind = ToInt64(intrinsic->kind()).value_or(kind);
      }
    }
    result += kind * (*object.substringStart - 1);
  }
  return result;
}

std::size_t ComputeOffsetsHelper::DoSymbol(
    Symbol &symbol, std::optional<const size_t> newAlign) {
  if (!symbol.has<ObjectEntityDetails>() && !symbol.has<ProcEntityDetails>()) {
    return 0;
  }
  SizeAndAlignment s{GetSizeAndAlignment(symbol, true)};
  if (s.size == 0) {
    return 0;
  }
  std::size_t previousOffset{offset_};
  size_t alignVal{newAlign.value_or(s.alignment)};
  offset_ = Align(offset_, alignVal);
  std::size_t padding{offset_ - previousOffset};
  symbol.set_size(s.size);
  symbol.set_offset(offset_);
  offset_ += s.size;
  alignment_ = std::max(alignment_, alignVal);
  return padding;
}

auto ComputeOffsetsHelper::GetSizeAndAlignment(
    const Symbol &symbol, bool entire) -> SizeAndAlignment {
  auto &targetCharacteristics{context_.targetCharacteristics()};
  if (IsDescriptor(symbol)) {
    auto dyType{evaluate::DynamicType::From(symbol)};
    const auto *derived{evaluate::GetDerivedTypeSpec(dyType)};
    int lenParams{derived ? CountLenParameters(*derived) : 0};
    bool needAddendum{derived || (dyType && dyType->IsUnlimitedPolymorphic())};

    // FIXME: Get descriptor size from targetCharacteristics instead
    // overapproximation
    std::size_t size{runtime::MaxDescriptorSizeInBytes(
        symbol.Rank(), needAddendum, lenParams)};

    return {size, targetCharacteristics.descriptorAlignment()};
  }
  if (IsProcedurePointer(symbol)) {
    return {targetCharacteristics.procedurePointerByteSize(),
        targetCharacteristics.procedurePointerAlignment()};
  }
  if (IsProcedure(symbol)) {
    return {};
  }
  auto &foldingContext{context_.foldingContext()};
  if (auto chars{evaluate::characteristics::TypeAndShape::Characterize(
          symbol, foldingContext)}) {
    if (entire) {
      if (auto size{ToInt64(chars->MeasureSizeInBytes(foldingContext))}) {
        return {static_cast<std::size_t>(*size),
            chars->type().GetAlignment(targetCharacteristics)};
      }
    } else { // element size only
      if (auto size{ToInt64(chars->MeasureElementSizeInBytes(
              foldingContext, true /*aligned*/))}) {
        return {static_cast<std::size_t>(*size),
            chars->type().GetAlignment(targetCharacteristics)};
      }
    }
  }
  return {};
}

// Align a size to its natural alignment, up to maxAlignment.
std::size_t ComputeOffsetsHelper::Align(std::size_t x, std::size_t alignment) {
  alignment =
      std::min(alignment, context_.targetCharacteristics().maxAlignment());
  return (x + alignment - 1) & -alignment;
}

void ComputeOffsets(SemanticsContext &context, Scope &scope) {
  ComputeOffsetsHelper{context}.Compute(scope);
}

} // namespace Fortran::semantics
