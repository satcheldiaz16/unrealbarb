#include "RhubarbLipSyncAsync.h"

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Interfaces/IPluginManager.h"

URhubarbLipSyncAsync* URhubarbLipSyncAsync::RunRhubarbAsync(
    UObject* WorldContextObject,
    const TArray<uint8>& PcmData,
    int32 SampleRate,
    const FString& DialogText)
{
    URhubarbLipSyncAsync* Node = NewObject<URhubarbLipSyncAsync>();
    Node->PcmRef        = PcmData;
    Node->SampleRateRef = SampleRate > 0 ? SampleRate : 24000;
    Node->DialogTextRef = DialogText;
    Node->RegisterWithGameInstance(WorldContextObject);
    return Node;
}

void URhubarbLipSyncAsync::Activate()
{
    if (PcmRef.Num() == 0)
    {
        FinishOnGameThread(false, {}, TEXT("Empty PCM data"));
        return;
    }

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [this, Pcm = MoveTemp(PcmRef), Rate = SampleRateRef, Dialog = DialogTextRef]()
    {
        TArray<uint8> WavBytes;
        BuildWavFromPcm(Pcm, Rate, /*NumChannels=*/1, WavBytes);

        TArray<FRhubarbMouthCue> Cues;
        FString RawJson, Error;
        const bool bOk = RunRhubarbProcess(WavBytes, Dialog, Cues, RawJson, Error);

        if (!bOk)
        {
            UE_LOG(LogTemp, Error, TEXT("Rhubarb: %s"), *Error);
        }

        AsyncTask(ENamedThreads::GameThread,
            [this, bOk, Cues = MoveTemp(Cues), RawJson, Error]()
        {
            FinishOnGameThread(bOk, Cues, bOk ? RawJson : Error);
        });
    });
}

void URhubarbLipSyncAsync::FinishOnGameThread(
    bool bSuccess,
    const TArray<FRhubarbMouthCue>& Cues,
    const FString& RawJsonOrError)
{
    if (bSuccess)
    {
        OnCompleted.Broadcast(Cues, RawJsonOrError);
    }
    else
    {
        OnFailed.Broadcast(Cues, RawJsonOrError);
    }
    SetReadyToDestroy();
}

void URhubarbLipSyncAsync::BuildWavFromPcm(
    const TArray<uint8>& Pcm, int32 SampleRate,
    int32 NumChannels, TArray<uint8>& OutWav)
{
    const uint16 BitsPerSample = 16;
    const uint32 DataSize   = Pcm.Num();
    const uint32 ByteRate   = SampleRate * NumChannels * (BitsPerSample / 8);
    const uint16 BlockAlign = NumChannels * (BitsPerSample / 8);
    const uint32 ChunkSize  = 36 + DataSize;

    OutWav.Reset(44 + DataSize);

    auto Push32 = [&OutWav](uint32 V) {
        OutWav.Add(V & 0xFF); OutWav.Add((V >> 8) & 0xFF);
        OutWav.Add((V >> 16) & 0xFF); OutWav.Add((V >> 24) & 0xFF);
    };
    auto Push16 = [&OutWav](uint16 V) {
        OutWav.Add(V & 0xFF); OutWav.Add((V >> 8) & 0xFF);
    };
    auto PushTag = [&OutWav](const char* T) {
        OutWav.Add(T[0]); OutWav.Add(T[1]); OutWav.Add(T[2]); OutWav.Add(T[3]);
    };

    PushTag("RIFF"); Push32(ChunkSize); PushTag("WAVE");
    PushTag("fmt "); Push32(16); Push16(1); // 1 = PCM integer
    Push16(static_cast<uint16>(NumChannels));
    Push32(static_cast<uint32>(SampleRate));
    Push32(ByteRate);
    Push16(BlockAlign); Push16(BitsPerSample);
    PushTag("data"); Push32(DataSize);
    OutWav.Append(Pcm);
}

bool URhubarbLipSyncAsync::RunRhubarbProcess(
    const TArray<uint8>& WavBytes,
    const FString& DialogText,
    TArray<FRhubarbMouthCue>& OutCues,
    FString& OutRawJson,
    FString& OutError)
{
    // Locate the exe inside THIS plugin, wherever the plugin is installed.
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealBarb"));
    if (!Plugin.IsValid())
    {
        OutError = TEXT("UnrealBarb plugin not found");
        return false;
    }

    const FString PluginBaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
    const FString ExePath = FPaths::Combine(
        PluginBaseDir, TEXT("ThirdParty/Rhubarb/rhubarb.exe"));

    if (!FPaths::FileExists(ExePath))
    {
        OutError = FString::Printf(TEXT("Rhubarb not found at %s"), *ExePath);
        return false;
    }

    const FString Stem = FGuid::NewGuid().ToString();

    // Absolute, normalized paths so Rhubarb's working dir can't break resolution.
    const FString TempDir    = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
    const FString WavPath    = FPaths::Combine(TempDir, Stem + TEXT(".wav"));
    const FString JsonPath   = FPaths::Combine(TempDir, Stem + TEXT(".json"));
    const FString DialogPath = FPaths::Combine(TempDir, Stem + TEXT(".txt"));

    ON_SCOPE_EXIT
    {
        IFileManager& FM = IFileManager::Get();
        FM.Delete(*WavPath,    false, true, true);
        FM.Delete(*JsonPath,   false, true, true);
        FM.Delete(*DialogPath, false, true, true);
    };

    if (!FFileHelper::SaveArrayToFile(WavBytes, *WavPath))
    {
        OutError = TEXT("Failed to write temp WAV");
        return false;
    }

    FString Args = FString::Printf(TEXT("-f json -o \"%s\""), *JsonPath);

    // Only pass --dialogFile if the write genuinely succeeded AND the file
    // exists on disk right now. Use UTF-8 so Rhubarb reads it cleanly.
    bool bDialogReady = false;
    if (!DialogText.IsEmpty())
    {
        if (FFileHelper::SaveStringToFile(
                DialogText, *DialogPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            && FPaths::FileExists(DialogPath))
        {
            bDialogReady = true;
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Rhubarb: dialog file could not be written; running without it"));
        }
    }

    if (bDialogReady)
    {
        Args += FString::Printf(TEXT(" --dialogFile \"%s\""), *DialogPath);
    }

    Args += FString::Printf(TEXT(" \"%s\""), *WavPath);

    int32 ReturnCode = -1;
    FString StdOut, StdErr;
    const FString WorkingDir = FPaths::GetPath(ExePath);

    const bool bLaunched = FPlatformProcess::ExecProcess(
        *ExePath, *Args, &ReturnCode, &StdOut, &StdErr, *WorkingDir);

    if (!bLaunched || ReturnCode != 0)
    {
        OutError = FString::Printf(
            TEXT("Rhubarb failed (launched=%d code=%d): %s"),
            bLaunched ? 1 : 0, ReturnCode, *StdErr);
        return false;
    }

    if (!FFileHelper::LoadFileToString(OutRawJson, *JsonPath))
    {
        OutError = TEXT("Could not read Rhubarb output JSON");
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutRawJson);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("Invalid Rhubarb JSON");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Cues = nullptr;
    if (Root->TryGetArrayField(TEXT("mouthCues"), Cues))
    {
        for (const TSharedPtr<FJsonValue>& Val : *Cues)
        {
            const TSharedPtr<FJsonObject> Obj = Val->AsObject();
            if (!Obj.IsValid()) continue;

            FRhubarbMouthCue Cue;
            Cue.Start = Obj->GetNumberField(TEXT("start"));
            Cue.End   = Obj->GetNumberField(TEXT("end"));
            Cue.Shape = Obj->GetStringField(TEXT("value"));
            OutCues.Add(Cue);
        }
    }

    return true;
}