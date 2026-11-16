// libkmodmd7.so: MobileDumper-7's engine analysis (MIT, https://github.com/MJx0/MobileDumper-7) run inside the game process
// to detect the engine's structure layouts. kmod loads it on demand (dlopen) and reads the JSON it writes. It is a separate
// library so that MobileDumper-7's own Unreal wrapper classes cannot clash with the generated SDK compiled into the core.
//
//   int kmod_md7_run(const char* package, const char* json_path, const char* log_path)   0 = ok
//
// The sequence is the one of FDumperMain::Run without the generators: module, analyzer, objects, names, settings, offsets.
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#include "Architecture/IArchDecoder.h"
#include "Generator/Generators/Generator.h"
#include "Memory/IMemory.h"
#include "Memory/MemoryAndroid.h"
#include "Profile/IProfile.h"
#include "Settings.h"
#include "Utils/Logger.h"

namespace
{
std::string Hex(long long v) { char b[32]; snprintf(b, sizeof(b), "%lld", v); return b; }
}

extern "C" __attribute__((visibility("default"))) int kmod_md7_run(const char* package, const char* jsonPath, const char* logPath)
{
    using namespace MD7::Globals;
    GLogger.SetFileStream(logPath ? logPath : "/dev/null");
    std::string err;
    auto fail = [&](const char* what) -> int
    {
        GLogger.FmtWrite(ELogLevel::Error, "kmod md7: {} failed: {}\n", what, err);
        GLogger.CloseFileStream();
        return 1;
    };

    std::string work = std::string(jsonPath ? jsonPath : "/data/local/tmp/layout.json") + ".work";
    GSettings.Generator.SDKGenerationPath = work;
    GMemory = std::make_unique<FMemoryAndroid>(getpid(), EK_MEM_OP_SYSCALL);
    static_cast<FMemoryAndroid*>(GMemory.get())->SetProcessRealName(package ? package : "");
    GArchDecoder = CreateArchDecoder(EArch::Arm64);
    GProfile = std::make_shared<IProfile>();

    if (!GMemory->Initialize() || !GMemory->IsMemoryAccessOk()) { err = "memory access"; return fail("memory"); }
    InternalSettings::GameName = GProfile->GetGameIdentifier();
    InternalSettings::GameVersion = GProfile->GetGameVersion();
    if (InternalSettings::GameName.empty()) InternalSettings::GameName = package ? package : "game";
    if (InternalSettings::GameVersion.empty()) InternalSettings::GameVersion = "unknown";

    if (!Generator::InitUnrealModule(err)) return fail("InitUnrealModule");
    if (!Generator::InitUEAnalyzerKitty(err)) return fail("InitUEAnalyzerKitty");
    GDecryptCallbacks = FDecryptCallbacks{};
    GProfile->OverrideDecryptCallbacks(GDecryptCallbacks);
    if (!Generator::InitObjects(err)) return fail("InitObjects");
    if (!Generator::InitNames(err)) return fail("InitNames");
    Generator::ReleaseAnalyzer();
    if (!Generator::InitSettings(err)) return fail("InitSettings");
    if (!Generator::InitOffsets(err)) return fail("InitOffsets");
    if (!Generator::InitInternalSettings(err)) return fail("InitInternalSettings");

    const auto& o = GOffsets;
    const auto& s = GInSDKOffsets;
    std::string j = "{\n";
    auto kv = [&](const char* k, long long v) { j += std::string("  \"") + k + "\": " + Hex(v) + ",\n"; };
    kv("FName.CompIdx", o.FName.CompIdx); kv("FName.Number", o.FName.Number); kv("FName.SizeOf", o.FName.SizeOf);
    kv("FField.Vft", o.FField.Vft); kv("FField.Class", o.FField.Class); kv("FField.Owner", o.FField.Owner);
    kv("FField.Next", o.FField.Next); kv("FField.Name", o.FField.Name);
    kv("FFieldClass.Name", o.FFieldClass.Name); kv("FFieldClass.CastFlags", o.FFieldClass.CastFlags);
    kv("UObject.Vft", o.UObject.Vft); kv("UObject.Flags", o.UObject.Flags); kv("UObject.Index", o.UObject.Index);
    kv("UObject.Class", o.UObject.Class); kv("UObject.Name", o.UObject.Name); kv("UObject.Outer", o.UObject.Outer);
    kv("UField.Next", o.UField.Next);
    kv("UEnum.Names", o.UEnum.Names);
    kv("UStruct.SuperStruct", o.UStruct.SuperStruct); kv("UStruct.Children", o.UStruct.Children);
    kv("UStruct.ChildProperties", o.UStruct.ChildProperties); kv("UStruct.Size", o.UStruct.Size);
    kv("UStruct.MinAlignment", o.UStruct.MinAlignment);
    kv("UFunction.FunctionFlags", o.UFunction.FunctionFlags); kv("UFunction.NumParams", o.UFunction.NumParams);
    kv("UFunction.ParamSize", o.UFunction.ParamSize); kv("UFunction.ExecFunction", o.UFunction.ExecFunction);
    kv("UClass.CastFlags", o.UClass.CastFlags); kv("UClass.ClassDefaultObject", o.UClass.ClassDefaultObject);
    kv("Property.ArrayDim", o.Property.ArrayDim); kv("Property.ElementSize", o.Property.ElementSize);
    kv("Property.PropertyFlags", o.Property.PropertyFlags); kv("Property.Offset_Internal", o.Property.Offset_Internal);
    kv("Property.SizeOf", o.Property.SizeOf);
    kv("ByteProperty.Enum", o.ByteProperty.Enum); kv("BoolProperty.Base", o.BoolProperty.Base);
    kv("ObjectProperty.PropertyClass", o.ObjectProperty.PropertyClass); kv("ClassProperty.MetaClass", o.ClassProperty.MetaClass);
    kv("StructProperty.Struct", o.StructProperty.Struct); kv("ArrayProperty.Inner", o.ArrayProperty.Inner);
    kv("DelegateProperty.SignatureFunction", o.DelegateProperty.SignatureFunction);
    kv("MapProperty.Base", o.MapProperty.Base); kv("SetProperty.ElementProp", o.SetProperty.ElementProp);
    kv("EnumProperty.Base", o.EnumProperty.Base);
    kv("Statics.GEngine", (long long)s.Statics.GEngine); kv("Statics.GWorld", (long long)s.Statics.GWorld);
    kv("Statics.GObjects", (long long)s.Statics.GObjects); kv("Statics.GNames", (long long)s.Statics.GNames);
    kv("Statics.PEIndex", (long long)s.Statics.PEIndex); kv("Statics.PEOffset", (long long)s.Statics.PEOffset);
    kv("FText.TextData", s.FText.TextData); kv("FText.SizeOf", s.FText.SizeOf);
    kv("ULevel.Actors", s.ULevel.Actors); kv("UDataTable.RowMap", s.UDataTable.RowMap);
    j += "  \"ok\": 1\n}\n";
    { std::ofstream f(jsonPath ? jsonPath : "/data/local/tmp/layout.json", std::ios::trunc); f << j; }
    GLogger.FmtWrite(ELogLevel::Info, "kmod md7: layout written to {}\n", jsonPath ? jsonPath : "");
    GLogger.CloseFileStream();
    return 0;
}
