#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "RhubarbLipSyncAsync.generated.h"

USTRUCT(BlueprintType)
struct FRhubarbMouthCue
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Rhubarb")
    float Start = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "Rhubarb")
    float End = 0.f;

    // Rhubarb mouth shape: A,B,C,D,E,F,G,H,X
    UPROPERTY(BlueprintReadOnly, Category = "Rhubarb")
    FString Shape;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FRhubarbCompleted,
    const TArray<FRhubarbMouthCue>&, MouthCues,
    const FString&, RawJson);

UCLASS()
class UNREALBARB_API URhubarbLipSyncAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /**
     * Runs Rhubarb lip sync on raw 16-bit signed mono PCM bytes + dialog text,
     * entirely off the game thread.
     * @param PcmData     Raw 16-bit signed PCM, mono, little-endian.
     * @param SampleRate  e.g. 24000 or 16000 to match your ElevenLabs format.
     * @param DialogText  Transcript string (improves recognition). May be empty.
     */
    UFUNCTION(BlueprintCallable, Category = "Rhubarb",
        meta = (BlueprintInternalUseOnly = "true",
                DisplayName = "Run Rhubarb Lip Sync (Async)",
                WorldContext = "WorldContextObject"))
    static URhubarbLipSyncAsync* RunRhubarbAsync(
        UObject* WorldContextObject,
        const TArray<uint8>& PcmData,
        int32 SampleRate,
        const FString& DialogText);

    UPROPERTY(BlueprintAssignable)
    FRhubarbCompleted OnCompleted;

    UPROPERTY(BlueprintAssignable)
    FRhubarbCompleted OnFailed;

    virtual void Activate() override;

private:
    TArray<uint8> PcmRef;
    int32 SampleRateRef = 24000;
    FString DialogTextRef;

    static void BuildWavFromPcm(const TArray<uint8>& Pcm, int32 SampleRate,
                                int32 NumChannels, TArray<uint8>& OutWav);

    static bool RunRhubarbProcess(const TArray<uint8>& WavBytes,
                                  const FString& DialogText,
                                  TArray<FRhubarbMouthCue>& OutCues,
                                  FString& OutRawJson,
                                  FString& OutError);

    void FinishOnGameThread(bool bSuccess,
                            const TArray<FRhubarbMouthCue>& Cues,
                            const FString& RawJsonOrError);
};