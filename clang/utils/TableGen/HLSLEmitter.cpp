//===--- HLSLEmitter.cpp - HLSL intrinsic header generator ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend generates hlsl_intrinsics_gen.inc (alias overloads)
// and hlsl_detail_intrinsics_gen.inc (inline/detail overloads) for HLSL
// intrinsic functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TableGen/Record.h"

using namespace llvm;

namespace {

/// Get the vector type name: elemtype + N (e.g., "float3").
static std::string getVecTypeName(StringRef ElemType, int N) {
  return (ElemType + Twine(N)).str();
}

/// Get the matrix type name: elemtype + R + "x" + C (e.g., "bool2x3").
static std::string getMatTypeName(StringRef ElemType, int R, int C) {
  return (ElemType + Twine(R) + "x" + Twine(C)).str();
}

/// Compute the return type for a given input type and shape.
/// RetPattern comes from HLSLReturnType.Pattern.
static std::string getReturnType(StringRef RetPattern, StringRef ElemType,
                                 int VecSize, int MatR, int MatC) {
  if (RetPattern == "same") {
    if (MatR > 0)
      return getMatTypeName(ElemType, MatR, MatC);
    if (VecSize > 0)
      return getVecTypeName(ElemType, VecSize);
    return ElemType.str();
  }
  if (RetPattern == "elemtype")
    return ElemType.str();
  if (RetPattern == "void")
    return "void";
  // Scalar-only return types (never expand to vector/matrix)
  if (RetPattern.starts_with("scalar_"))
    return RetPattern.drop_front(7).str();
  // Fixed type name, possibly with shape suffix already (e.g., "uint4")
  // or a bare element type to be shaped like the input.
  // If no shape suffix and input has shape, apply input's shape.
  if (MatR > 0)
    return getMatTypeName(RetPattern, MatR, MatC);
  if (VecSize > 0)
    return getVecTypeName(RetPattern, VecSize);
  return RetPattern.str();
}

/// Get the argument type string for a given position.
static std::string getArgType(StringRef ElemType, int VecSize, int MatR,
                               int MatC) {
  if (MatR > 0)
    return getMatTypeName(ElemType, MatR, MatC);
  if (VecSize > 0)
    return getVecTypeName(ElemType, VecSize);
  return ElemType.str();
}

/// Emit a single overload declaration.
/// FixedArgs maps arg index to a fixed type name (empty = varying).
/// ArgKind[i]: 0=Varying, 1=VaryingElemType, 2=VaryingShape.
/// ArgShapeElemType[i]: element type name for VaryingShape args.
/// DetailFunc: if non-empty, emit inline function body calling
/// __detail::DetailFunc(args...) instead of _HLSL_BUILTIN_ALIAS.
/// Body: if non-empty, emit inline function with this literal body text.
/// ParamNames: custom parameter names; falls back to p0, p1, ... if empty.
static void emitOverload(raw_ostream &OS, StringRef Builtin,
                         StringRef DetailFunc, StringRef Body,
                         StringRef FuncName,
                         StringRef RetPattern, ArrayRef<StringRef> FixedArgs,
                         ArrayRef<int> ArgKind,
                         ArrayRef<StringRef> ArgShapeElemType,
                         ArrayRef<StringRef> ParamNames, int NumArgs,
                         StringRef ArgElemType, int VecSize, int MatR, int MatC,
                         bool IsConstexpr, bool IsConvergent) {
  std::string RetType =
      getReturnType(RetPattern, ArgElemType, VecSize, MatR, MatC);
  std::string VaryingType = getArgType(ArgElemType, VecSize, MatR, MatC);

  bool IsDetail = !DetailFunc.empty();
  bool IsInline = !Body.empty();
  bool HasBody = IsDetail || IsInline;
  bool HasParamNames = !ParamNames.empty() || HasBody;

  // Helper to get the parameter name for argument I.
  auto GetParamName = [&](int I) -> std::string {
    if (I < static_cast<int>(ParamNames.size()) && !ParamNames[I].empty())
      return ParamNames[I].str();
    return ("p" + Twine(I)).str();
  };

  if (!HasBody)
    OS << "_HLSL_BUILTIN_ALIAS(" << Builtin << ")\n";
  if (IsConvergent)
    OS << "__attribute__((convergent)) ";
  if (HasBody) {
    if (IsConstexpr)
      OS << "constexpr ";
    else
      OS << "inline ";
  }
  OS << RetType << " " << FuncName << "(";
  for (int I = 0; I < NumArgs; ++I) {
    if (I > 0)
      OS << ", ";
    std::string ArgTy;
    if (!FixedArgs[I].empty())
      ArgTy = FixedArgs[I].str();
    else if (ArgKind[I] == 1) // VaryingElemType
      ArgTy = ArgElemType.str();
    else if (ArgKind[I] == 2) // VaryingShape
      ArgTy = getArgType(ArgShapeElemType[I], VecSize, MatR, MatC);
    else
      ArgTy = VaryingType;
    OS << ArgTy;
    if (HasParamNames)
      OS << " " << GetParamName(I);
  }
  if (IsDetail) {
    OS << ") {\n  return __detail::" << DetailFunc << "(";
    for (int I = 0; I < NumArgs; ++I) {
      if (I > 0)
        OS << ", ";
      OS << GetParamName(I);
    }
    OS << ");\n}\n";
  } else if (IsInline) {
    OS << ") { " << Body << " }\n";
  } else {
    OS << ");\n";
  }
}

/// Emit availability prefix for a single overload.
static void emitAvailability(raw_ostream &OS, StringRef Version) {
  OS << "_HLSL_AVAILABILITY(shadermodel, " << Version << ")\n";
}

static void emit16BitAvailability(raw_ostream &OS, StringRef Version) {
  OS << "_HLSL_16BIT_AVAILABILITY(shadermodel, " << Version << ")\n";
}

/// Get version string from a ShaderModel record. Returns "" for NoSM.
static std::string getVersionString(const Record *SM) {
  int Major = SM->getValueAsInt("Major");
  int Minor = SM->getValueAsInt("Minor");
  if (Major == 0 && Minor == 0)
    return "";
  return (Twine(Major) + "." + Twine(Minor)).str();
}

/// Holds the properties of an element type extracted from an HLSLType record.
struct ElemTypeInfo {
  StringRef Name;
  bool Is16Bit;
  bool IsConditionally16Bit;
};

/// Extract ElemTypeInfo from an HLSLType record.
static ElemTypeInfo getElemTypeInfo(const Record *R) {
  return {R->getValueAsString("Name"), R->getValueAsBit("Is16Bit"),
          R->getValueAsBit("IsConditionally16Bit")};
}

/// Process a single HLSLBuiltin record and emit all its overloads.
/// If EmitDetail is true, only emit builtins with DetailFunc set.
/// If EmitDetail is false, only emit builtins without DetailFunc (alias + Body modes).
static void emitBuiltin(raw_ostream &OS, const Record *R, bool EmitDetail) {
  StringRef FuncName = R->getValueAsString("Name");
  StringRef Builtin = R->getValueAsString("Builtin");
  StringRef DetailFunc = R->getValueAsString("DetailFunc");
  StringRef Body = R->getValueAsString("Body");
  bool IsConstexpr = R->getValueAsBit("IsConstexpr");

  // Read custom parameter names.
  auto ParamNameRecords = R->getValueAsListOfStrings("ParamNames");
  SmallVector<StringRef, 4> ParamNames(ParamNameRecords.begin(),
                                       ParamNameRecords.end());

  // Filter: alias .inc gets only pure BUILTIN_ALIAS records (no DetailFunc,
  // no Body). Detail .inc gets everything with an inline body (DetailFunc or
  // Body), since those may depend on helpers or other declarations.
  bool HasInlineBody = !DetailFunc.empty() || !Body.empty();
  if (EmitDetail && !HasInlineBody)
    return;
  if (!EmitDetail && HasInlineBody)
    return;
  bool VaryingScalar = R->getValueAsBit("VaryingScalar");
  bool IsConvergent = R->getValueAsBit("IsConvergent");
  const Record *AvailRec = R->getValueAsDef("Availability");
  std::string DefaultAvail = getVersionString(AvailRec);

  // Read the return type pattern from the HLSLReturnType record
  const Record *RetTypeRec = R->getValueAsDef("ReturnType");
  StringRef RetPattern = RetTypeRec->getValueAsString("Pattern");

  auto VaryingVecSizes = R->getValueAsListOfInts("VaryingVecSizes");
  auto VaryingMatDims = R->getValueAsListOfDefs("VaryingMatDims");

  // Read Args list. Each arg is one of:
  //   Varying         — type varies with the overload (empty FixedArgs entry)
  //   VaryingElemType — scalar element of the varying type
  //   VaryingShape    — varying shape but with a fixed element type
  //   ScalarType/VectorType — fully fixed type
  auto ArgRecords = R->getValueAsListOfDefs("Args");
  int NumArgs = ArgRecords.size();

  // Per-arg metadata for emitOverload:
  //   FixedArgs[i] = "" means varying, non-empty means fixed type string
  //   ArgKind[i]:  0 = Varying, 1 = VaryingElemType, 2 = VaryingShape
  enum ArgKindEnum { AK_Varying = 0, AK_ElemType = 1, AK_VaryingShape = 2 };
  SmallVector<StringRef, 4> FixedArgs;
  SmallVector<int, 4> ArgKind;
  SmallVector<StringRef, 4> ArgShapeElemType; // element type for VaryingShape
  bool Has16BitIntArg = false;
  for (const auto *Arg : ArgRecords) {
    if (Arg->isSubClassOf("VectorType")) {
      FixedArgs.push_back(Arg->getValueAsString("TypeName"));
      ArgKind.push_back(AK_Varying);
      ArgShapeElemType.push_back("");
      const Record *BaseType = Arg->getValueAsDef("BaseType");
      if (BaseType->getValueAsBit("Is16Bit"))
        Has16BitIntArg = true;
    } else if (Arg->isSubClassOf("HLSLType")) {
      // HLSLType used directly as a fixed scalar argument.
      FixedArgs.push_back(Arg->getValueAsString("TypeName"));
      ArgKind.push_back(AK_Varying);
      ArgShapeElemType.push_back("");
      if (Arg->getValueAsBit("Is16Bit"))
        Has16BitIntArg = true;
    } else if (Arg->isSubClassOf("VaryingShape")) {
      FixedArgs.push_back("");
      ArgKind.push_back(AK_VaryingShape);
      ArgShapeElemType.push_back(Arg->getValueAsString("TypeName"));
    } else if (Arg->getName() == "VaryingElemType") {
      FixedArgs.push_back("");
      ArgKind.push_back(AK_ElemType);
      ArgShapeElemType.push_back("");
    } else {
      FixedArgs.push_back("");
      ArgKind.push_back(AK_Varying);
      ArgShapeElemType.push_back("");
    }
  }

  // Read VaryingTypes — each is an HLSLType record
  auto ElemTypeRecords = R->getValueAsListOfDefs("VaryingTypes");
  SmallVector<ElemTypeInfo, 8> ElemTypes;
  for (const auto *ET : ElemTypeRecords)
    ElemTypes.push_back(getElemTypeInfo(ET));

  // Categorize element types using record properties
  SmallVector<ElemTypeInfo, 4> HalfTypes;
  SmallVector<ElemTypeInfo, 8> SixteenBitIntTypes;
  SmallVector<ElemTypeInfo, 8> RegularTypes;

  for (const auto &ETI : ElemTypes) {
    if (ETI.IsConditionally16Bit)
      HalfTypes.push_back(ETI);
    else if (ETI.Is16Bit)
      SixteenBitIntTypes.push_back(ETI);
    else
      RegularTypes.push_back(ETI);
  }

  // Emit documentation comment if present
  StringRef Doc = R->getValueAsString("Doc");
  if (!Doc.empty()) {
    // Strip leading/trailing whitespace from the doc block
    Doc = Doc.trim();
    // Each line gets a "/// " prefix
    SmallVector<StringRef, 16> DocLines;
    Doc.split(DocLines, '\n');
    for (StringRef Line : DocLines) {
      if (Line.empty())
        OS << "///\n";
      else
        OS << "/// " << Line << "\n";
    }
  }

  // Emit section comment
  OS << "// " << FuncName << " overloads\n";

  // If VaryingTypes is empty, emit a single overload with no type expansion.
  if (ElemTypes.empty()) {
    if (Has16BitIntArg)
      OS << "#ifdef __HLSL_ENABLE_16_BIT\n";
    if (!DefaultAvail.empty())
      emitAvailability(OS, DefaultAvail);
    emitOverload(OS, Builtin, DetailFunc, Body, FuncName, RetPattern, FixedArgs, ArgKind, ArgShapeElemType, ParamNames, NumArgs, "", 0,
                 0, 0, IsConstexpr, IsConvergent);
    if (Has16BitIntArg)
      OS << "#endif\n";
    OS << "\n";
    return;
  }

  // Lambda to emit all shape overloads for a type
  auto EmitWithAvail = [&](const ElemTypeInfo &ETI, StringRef Avail,
                           bool Use16BitMacro) {
    auto EmitOne = [&](int VecSize, int MatR, int MatC) {
      if (!Avail.empty()) {
        if (Use16BitMacro)
          emit16BitAvailability(OS, Avail);
        else
          emitAvailability(OS, Avail);
      }
      emitOverload(OS, Builtin, DetailFunc, Body, FuncName, RetPattern, FixedArgs, ArgKind, ArgShapeElemType, ParamNames, NumArgs,
                   ETI.Name, VecSize, MatR, MatC, IsConstexpr, IsConvergent);
    };

    if (VaryingScalar)
      EmitOne(0, 0, 0);
    for (int64_t N : VaryingVecSizes)
      EmitOne(N, 0, 0);
    for (const auto *MD : VaryingMatDims)
      EmitOne(0, MD->getValueAsInt("Rows"), MD->getValueAsInt("Cols"));
  };

  // Helper to check if a version is >= SM6.2 (16-bit types already supported).
  auto isAtLeastSM6_2 = [](StringRef Version) -> bool {
    return Version >= "6.2";
  };

  // 1. Emit half-like types. Use _HLSL_16BIT_AVAILABILITY only when the
  // default availability is below SM6.2. When the intrinsic already requires
  // SM6.2+ (i.e. DefaultAvail >= 6.2), 16-bit support is implied so the
  // standard _HLSL_AVAILABILITY macro is correct.
  bool DefaultIsAtLeastSM6_2 = isAtLeastSM6_2(DefaultAvail);
  for (const auto &ETI : HalfTypes) {
    StringRef Avail = DefaultIsAtLeastSM6_2 ? DefaultAvail : StringRef("6.2");
    EmitWithAvail(ETI, Avail, /*Use16BitMacro=*/!DefaultIsAtLeastSM6_2);
  }

  if (!HalfTypes.empty() &&
      (!SixteenBitIntTypes.empty() || !RegularTypes.empty()))
    OS << "\n";

  // 2. Emit 16-bit int types inside #ifdef __HLSL_ENABLE_16_BIT
  if (!SixteenBitIntTypes.empty()) {
    OS << "#ifdef __HLSL_ENABLE_16_BIT\n";
    StringRef SixteenBitAvail =
        DefaultIsAtLeastSM6_2 ? DefaultAvail : StringRef("6.2");
    for (const auto &ETI : SixteenBitIntTypes)
      EmitWithAvail(ETI, SixteenBitAvail, /*Use16BitMacro=*/false);
    OS << "#endif\n";

    if (!RegularTypes.empty())
      OS << "\n";
  }

  // 3. Emit regular type overloads
  for (const auto &ETI : RegularTypes) {
    if (!DefaultAvail.empty()) {
      EmitWithAvail(ETI, DefaultAvail, /*Use16BitMacro=*/false);
    } else {
      auto EmitOne = [&](int VecSize, int MatR, int MatC) {
        emitOverload(OS, Builtin, DetailFunc, Body, FuncName, RetPattern, FixedArgs, ArgKind, ArgShapeElemType, ParamNames, NumArgs,
                     ETI.Name, VecSize, MatR, MatC, IsConstexpr, IsConvergent);
      };
      if (VaryingScalar)
        EmitOne(0, 0, 0);
      for (int64_t N : VaryingVecSizes)
        EmitOne(N, 0, 0);
      for (const auto *MD : VaryingMatDims)
        EmitOne(0, MD->getValueAsInt("Rows"), MD->getValueAsInt("Cols"));
    }
  }

  OS << "\n";
}

} // anonymous namespace

namespace clang {

void EmitHLSLIntrinsics(const RecordKeeper &Records, raw_ostream &OS) {
  OS << "// This file is auto-generated by clang-tblgen from "
        "HLSLIntrinsics.td.\n";
  OS << "// Do not edit this file directly.\n\n";

  auto Builtins = Records.getAllDerivedDefinitions("HLSLBuiltin");

  // Emit builtin-alias declarations (no DetailFunc, no Body).
  for (const auto *R : Builtins)
    emitBuiltin(OS, R, /*EmitDetail=*/false);
}

void EmitHLSLDetailIntrinsics(const RecordKeeper &Records, raw_ostream &OS) {
  OS << "// This file is auto-generated by clang-tblgen from "
        "HLSLIntrinsics.td.\n";
  OS << "// Do not edit this file directly.\n\n";

  auto Builtins = Records.getAllDerivedDefinitions("HLSLBuiltin");

  // Emit inline intrinsics (detail functions and inline bodies).
  for (const auto *R : Builtins)
    emitBuiltin(OS, R, /*EmitDetail=*/true);
}

} // namespace clang
