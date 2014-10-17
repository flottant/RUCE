#include "Synth.h"
#include "Verbose.h"

static int LoadHNMFrame(List_HNMFrame* Dest, List_DataFrame* DestPhse,
    RUCE_DB_Entry* Sorc, int WinSize)
{
    Verbose(4, "(function entrance)\n");
    int FrameNum = Sorc -> FrameList_Index + 1;
    RCall(List_HNMFrame, Resize)(Dest, FrameNum);
    RCall(List_DataFrame, Resize)(DestPhse, FrameNum);
    Verbose(4, "%d HNMFrames were found.\n", FrameNum);
    
    for(int i = 0; i < FrameNum; i ++)
    {
        int HNum;
        
        RUCE_DB_Frame* SorcFrame = & Sorc -> FrameList[i];
        HNMFrame* DestFrame = & Dest -> Frames[i];
        HNum = SorcFrame -> Freq_Index + 1;
        
        RCall(HNMFrame, Resize)(DestFrame, WinSize, HNum);
        RCall(DataFrame, Resize)(& DestPhse -> Frames[i], HNum);
        
        for(int j = 0; j < HNum; j ++)
        {
            DestFrame -> Hmnc.Freq[j] = SorcFrame -> Freq[j];
            DestFrame -> Hmnc.Ampl[j] = SorcFrame -> Ampl[j];
            DestPhse -> Frames[i].Data[j] = SorcFrame -> Phse[j];
        }
        
        RCall(CDSP2_VSet, Real)(DestFrame -> Noiz, -999, WinSize / 2 + 1);
        RCall(CDSP2_Resample_Linear, Real)(DestFrame -> Noiz, SorcFrame -> Noiz,
            WinSize / 2 + 1, Sorc -> NoizSize);
    }
    
    return 1;
}

static void InterpFetchHNMFrame(HNMFrame* Dest, List_HNMFrame* Sorc,
    Transition* Trans)
{
    int LIndex, HIndex;
    LIndex = Trans -> LowerIndex;
    HIndex = Trans -> LowerIndex == Sorc -> Frames_Index ? LIndex : LIndex + 1;
    HNMFrame* LHNM = & Sorc -> Frames[LIndex];
    HNMFrame* HHNM = & Sorc -> Frames[HIndex];
    
    RCall(HNMFrame, InterpFrom)(Dest, LHNM, HHNM, Trans -> X);
}

static void InterpFetchPhase(DataFrame* Dest, List_DataFrame* Sorc,
    Transition* Trans)
{
    int LIndex, HIndex;
    LIndex = Trans -> LowerIndex;
    HIndex = Trans -> LowerIndex == Sorc -> Frames_Index ? LIndex : LIndex + 1;
    DataFrame* LPhase = & Sorc -> Frames[LIndex];
    DataFrame* HPhase = & Sorc -> Frames[HIndex];
    
    DataFrame SLPhase, SHPhase;
    RCall(DataFrame, Ctor)(& SLPhase);
    RCall(DataFrame, Ctor)(& SHPhase);
    
    RCall(DataFrame, From)(& SLPhase, LPhase);
    RCall(DataFrame, From)(& SHPhase, HPhase);
    RCall(CSVP_PhaseSyncH, Real)(& SLPhase, 0);
    RCall(CSVP_PhaseSyncH, Real)(& SHPhase, 0);
    RCall(CSVP_PhaseInterp, Real)(Dest, & SLPhase, & SHPhase, Trans -> X);
    
    RCall(DataFrame, Dtor)(& SLPhase);
    RCall(DataFrame, Dtor)(& SHPhase);
}

#define Sample2Sec(x) ((double)(x) / SampleRate)
#define Sec2Sample(x) (SampleRate * (x))
static int MatchUnitToNote(PMatch* Dest, RUCE_DB_Entry* SorcDB,
    RUCE_Session* Session, int NoteIndex, double DTV)
{
    Verbose(4, "(function entrance)\n");
    int SampleRate = Session -> SampleRate;
    RUCE_Note* Note = & Session -> NoteList[NoteIndex];
    
    //ST0 -> DT0, STV -> DTV, STD -> DTD
    double STV = SorcDB -> VOT;
    double STD = Sample2Sec(TopOf(SorcDB -> FrameList).Position);
    double DTD = Note -> Duration + DTV;
    RCall(PMatch, AddPair)(Dest, 0, 0);
    RCall(PMatch, AddPair)(Dest, DTV, STV);
    RCall(PMatch, AddPair)(Dest, DTD, STD);
    
    //ST1 -> DT1, ST2 -> DT2
    double SP1 = SorcDB -> InvarLeft - STV;
    double SP2 = SorcDB -> InvarRight - STV;
    double DP1, DP2;
    
    if(Note -> CParamSet.DurInitial > 0)
        DP1 = Note -> CParamSet.DurInitial;
    if(Note -> CParamSet.DurFinal > 0)
        DP2 = STD - Note -> CParamSet.DurFinal - STV;
    
    //Final not determined yet.
    if(Note -> CParamSet.DurInitial > 0 && Note -> CParamSet.DurFinal <= 0)
    {
        DP2 = DTD - (STD - SP2);
        //Prevent over-long finals.
        if(DP2 - DP1 < DTD - DP2)
            DP2 = DP1 + (DTD - DP1) / 2.0;
    }
    
    //Initial not determined yet.
    if(Note -> CParamSet.DurFinal > 0 && Note -> CParamSet.DurInitial <= 0)
    {
        DP1 = SP1;
        //Prevent over-long initials.
        if(DP1 > DP2 - DP1)
            DP1 = DP2 / 2;
    }
    
    //Both final & initial not determined yet.
    if(Note -> CParamSet.DurFinal <= 0 && Note -> CParamSet.DurInitial <= 0)
    {
        DP1 = SP1;
        DP2 = DTD - (STD - SP2);
        
        if(DP2 < DP1)
        {
            DP1 = DP2 = SP1 / (SP1 + SP2) * DTD;
            DP1 *= 0.7;
            DP2 = DTD - (DTD - DP2) * 0.7;
        }
    }
    
    RCall(PMatch, AddPair)(Dest, DTV + DP1, STV + SP1);
    RCall(PMatch, AddPair)(Dest, DTV + DP2, STV + SP2);
    return 1;
}

static void LoadPulseTrain(List_Int* Dest, RUCE_DB_Entry* Sorc)
{
    Verbose(4, "(function entrance)\n");
    Dest -> Frames_Index = Sorc -> FrameList_Index;
    for(int i = 0; i <= Sorc -> FrameList_Index; i ++)
        Dest -> Frames[i] = Sorc -> FrameList[i].Position;
}

//Returns the alignment in index.
//  1: Success
//  0: Cannot load HNMFrame
// -1: Cannot match phoneme durations
int RUCE_VoicedToHNMContour(List_HNMContour* Dest, List_DataFrame* DestPhse,
    RUCE_DB_Entry* SorcDB, CSVP_PitchModel* SorcPM, RUCE_Session* Session,
    int NoteIndex)
{
    RUCE_SessionConfig* Config = Session -> Config;
    RUCE_Note* Note = & Session -> NoteList[NoteIndex];
    double NoteTime = Session -> TimeList[NoteIndex];
    int SampleRate = Session -> SampleRate;
    
    List_HNMFrame  SorcHNM;
    List_DataFrame SorcPhase;
    List_Int       SorcTrain;
    PMatch         SorcTrainMatch;
    RCall(List_HNMFrame, Ctor)(& SorcHNM);
    RCall(List_DataFrame, Ctor)(& SorcPhase);
    
    /*
        Step1: Load
    */
    if(LoadHNMFrame(& SorcHNM, & SorcPhase, SorcDB, Config -> WinSize) < 1)
    {
        Verbose(1, "[Error] Cannot load HNMFrame.\n");
        RDelete(& SorcHNM, & SorcPhase);
        return 0;
    }
    RCall(List_Int, CtorSize)(& SorcTrain, SorcDB -> FrameList_Index + 1);
    LoadPulseTrain(& SorcTrain, SorcDB);
    RCall(PMatch, Ctor)(& SorcTrainMatch);
    RCall(CDSP2_List_Int_ToPMatch, Real)(& SorcTrain, & SorcTrainMatch);
    RCall(List_Int, Dtor)(& SorcTrain);
    
    /*
        Step2: Match
                         (SP1)  (SP2)
              0 ST0    STV  ST1     ST2    STD
        Src   |--|------|----|-------|------|
                         (DP1)  (DP2)
            0  DT0     DTV   DT1     DT2     DTD
        Dst |---|-------|-----|-------|-------|
        
        ST0/DT0: First HNM frame
        ST1/DT1: Initial of voiced part
        ST2/DT2: Final of voiced part
        STV/DTV: VOT/Syllable alignment
        STD/DTD: End/Duration
    */
    int SorcHead = SorcDB -> FrameList[0].Position;
    int HopSize  = SorcDB -> HopSize;
    double STV = SorcDB -> VOT;
    double DTV = Note -> CParamSet.DurConsonant <= 0 ?
        SorcDB -> VOT : Note -> CParamSet.DurConsonant;
    
    PMatch TimeMatch, RevTimeMatch;
    RCall(PMatch, Ctor)(& TimeMatch);
    RCall(PMatch, Ctor)(& RevTimeMatch);
    if(MatchUnitToNote(& TimeMatch, SorcDB, Session, NoteIndex, DTV) < 1)
    {
        Verbose(1, "[Error] Cannot match phoneme durations.\n");
        RDelete(& SorcHNM, & SorcPhase, & TimeMatch, & RevTimeMatch,
            & SorcTrainMatch);
        return -1;
    }
    RCall(PMatch, InvertTo)(& TimeMatch, & RevTimeMatch);
    //RCall(PMatch, Print)(& RevTimeMatch);
    
    double ST0 = Sample2Sec(SorcHead);
    double DT0 = RCall(PMatch, Query)(& RevTimeMatch, ST0).Y;
    double STD = Sample2Sec(TopOf(SorcDB -> FrameList).Position);
    double DTD = Note -> Duration + DTV;
    Verbose(5, "ST0=%f, STV=%f, STD=%f, DT0=%f, DTV=%f, DTD=%f\n",
        ST0, STV, STD, DT0, DTV, DTD);
    
    #define MapToSource(n) \
        Sec2Sample(RCall(PMatch, Query)(& TimeMatch, Sample2Sec(n) + DT0).Y)
    #define MapToGlobal(n) \
        (Sample2Sec(n) + DT0 - DTV + NoteTime)
    
    /*
        Step3: Transform
    */
    HNMFrame TempFrame;
    HNMContour TempContour;
    DataFrame TempPhase;
    RCall(HNMFrame, Ctor)(& TempFrame);
    RCall(HNMContour, Ctor)(& TempContour);
    RCall(DataFrame, Ctor)(& TempPhase);
    int Position = 0;
    int LocalDuration = SampleRate * (DTD - DT0);
    while(Position < LocalDuration)
    {
        int SourcePosition = MapToSource(Position);
        Transition Trans   = RCall(PMatch, Query)(& SorcTrainMatch,
            SourcePosition);
        double GlobalTime  = MapToGlobal(Position);
        Verbose(6, "%d %d %f %s\n", Position, Trans.LowerIndex, GlobalTime,
            Sample2Sec(Position) > DTV ? "V" : "U");
        
        double F0 = RCall(PMatch, Query)(Session -> FreqMatch, GlobalTime).Y;
        double Ampl = RCall(PMatch, Query)(Session -> AmplMatch, GlobalTime).Y;
        double Bre = RCall(PMatch, Query)(Session -> BreMatch, GlobalTime).Y;
        double Gen = RCall(PMatch, Query)(Session -> GenderMatch, GlobalTime).Y;
        Verbose(6, "F0=%f, Ampl=%f, Bre=%f, Gen=%f\n", F0, Ampl, Bre, Gen);
        
        InterpFetchHNMFrame(& TempFrame, & SorcHNM, & Trans);
        InterpFetchPhase(& TempPhase, & SorcPhase, & Trans);
        
        Position += HopSize;
    }
    RDelete(& TempFrame, & TempContour, & TempPhase);
    
    /*
        Step4: Post-transform
    */
    
    RDelete(& SorcHNM, & SorcPhase, & TimeMatch, & RevTimeMatch,
        & SorcTrainMatch);
    return 1;
}
