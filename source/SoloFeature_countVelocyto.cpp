#include "SoloFeature.h"
#include "streamFuns.h"
#include "TimeFunctions.h"
#include "SequenceFuns.h"
#include "SoloCommon.h"
#include <unordered_map>
#include <bitset>

void SoloFeature::countVelocyto(SoloFeature &soloFeatGene)
{//velocyto counting gets info from Gene counting
    time_t rawTime;

    vector<uint32> indWL(pSolo.cbWLsize, (uint32)-1); //index of WL in detected CB, i.e. reverse of indCB
    for (uint32 ii=0; ii<nCB; ii++)
        indWL[indCB[ii]]=ii;

    vector<unordered_map<typeUMI,vector<trTypeStruct>>> cuTrTypes (nCB);
    for (uint32 ii=0; ii<nCB; ii++)
        cuTrTypes[ii].reserve(readFeatSum->cbReadCount[ii] > 100 ? readFeatSum->cbReadCount[ii] : readFeatSum->cbReadCount[ii]/5); //heuristics...
    
    time(&rawTime);
    P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Velocyto counting: allocated arrays" <<endl;
    
    //////////// input records
    for (int iThread=0; iThread<P.runThreadN; iThread++) {//TODO: this can be parallelized
        fstream *streamReads = readFeatAll[iThread]->streamReads;
        streamReads->flush();
        streamReads->seekg(0,ios::beg);
        
        uint64 iread;
        while (*streamReads >> iread) {//until the end of file
            uint64 cb=soloFeatAll[pSolo.featureInd[SoloFeatureTypes::Gene]]->readInfo[iread].cb;
            if (cb+1==0) {//TODO: put a filter on CBs here, e.g. UMI threshold
                streamReads->ignore((uint32)-1, '\n');
                continue;
            };            
            uint32 iCB=indWL[cb];
            typeUMI umi=soloFeatAll[pSolo.featureInd[SoloFeatureTypes::Gene]]->readInfo[iread].umi;
            
            if (cuTrTypes[iCB].count(umi)>0 && cuTrTypes[iCB][umi].empty()) {//intersection is empty, no need to load this transcript
                streamReads->ignore((uint32)-1, '\n');
                continue;
            };

            uint32 nTr;
            *streamReads >> nTr;
            vector<trTypeStruct> trT(nTr);
            for (auto & tt: trT) {
                uint32 ty;
                *streamReads >> tt.tr >> ty;
                tt.type=(uint8) ty;
            };

            if (cuTrTypes[iCB].count(umi)==0) {//1st entry for this umi
                cuTrTypes[iCB][umi]=trT;
                continue;
            };
            
            uint32 inew=0;
            vector<trTypeStruct> trT1;
            trT1.reserve(cuTrTypes[iCB][umi].size());
            
            for (uint32 iold=0; iold<cuTrTypes[iCB][umi].size(); iold++) {//intersection of old with new
                while (cuTrTypes[iCB][umi][iold].tr>trT[inew].tr) //move through the sorted lists
                    ++inew;
                
                if (cuTrTypes[iCB][umi][iold].tr == trT[inew].tr) {//
                    trT1.push_back({trT[inew].tr, (uint8)(cuTrTypes[iCB][umi][iold].type | trT[inew].type)});
                };
            };
            cuTrTypes[iCB][umi]=trT1;//replace with intersection
        };        
    };   
    
    P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Velocyto counting: finished input" <<endl;

    
    //////////////////////////////////////////////////////////////////////////////
    /////////////////////////// counts for  each CB
    nUMIperCB.resize(nCB,0);
    nGenePerCB.resize(nCB,0);
       
    countMatStride=4; //hard-coded for now, works for both Gene*/SJ and Velocyto
    countCellGeneUMI.resize(nReadsMapped*countMatStride/5); //5 is heuristic, will be resized if needed
    countCellGeneUMIindex.resize(nCB+1);
    countCellGeneUMIindex[0]=0;
    
    for (uint32 iCB=0; iCB<nCB; iCB++) {//main collapse cycle
        map<uint32,array<uint32,3>> geneC;
        for (auto &umi : cuTrTypes[iCB]) {//cycle over UMIs
            if (umi.second.empty()) //no transcripts in the intesect
                continue;
            uint32 geneI=Trans.trGene[umi.second[0].tr];
            uint32 geneT=0;
            for (auto &tt : umi.second) {
                if (Trans.trGene[tt.tr] != geneI) {//multigene
                    geneI=(uint32)-1;
                    break;
                };
                geneT |= tt.type;
            };
            
            if (geneI+1==0) //multigene
                continue;

            bitset<velocytoTypeGeneBits> gV (geneT);
            if (!gV.test(AlignVsTranscript::ExonIntronSpan)) {//all UMIs are spanning models
                geneC[geneI][1]++; //unspliced 
            } else if (gV.test(AlignVsTranscript::Concordant)) {//>=1 purely exonic tr 
                if (!gV.test(AlignVsTranscript::Intron) && !gV.test(AlignVsTranscript::ExonIntron)) {//0 purely intronic && 0 mixed
                    geneC[geneI][0]++; //spliced 
                } else {//>=1 purely exonic and >=1 purely intronic or mixed
                    geneC[geneI][2]++; //ambiguous
                };
            } else {//0 exonic, >=1 intronic and/or >=1 mixed
                geneC[geneI][1]++;//unspliced
            };
            
            nUMIperCB[iCB]++;
        };
        
        countCellGeneUMIindex[iCB+1] = countCellGeneUMIindex[iCB];        
        
        if (nUMIperCB[iCB]==0) //no UMIs counted for this CB
            continue;
        
        nGenePerCB[iCB]+=geneC.size();
        readFeatSum->stats.V[readFeatSum->stats.nUMIs] += nUMIperCB[iCB];
        ++readFeatSum->stats.V[readFeatSum->stats.nCellBarcodes];
        
        if (countCellGeneUMI.size() < countCellGeneUMIindex[iCB+1] + geneC.size()*countMatStride) //allocated vector too small
            countCellGeneUMI.resize(countCellGeneUMI.size()*2);        
        
        for (auto & gg : geneC) {
            countCellGeneUMI[countCellGeneUMIindex[iCB+1]+0]=gg.first;
            for (uint32 ii=0;ii<3;ii++)
                countCellGeneUMI[countCellGeneUMIindex[iCB+1]+1+ii]=gg.second[ii];
            countCellGeneUMIindex[iCB+1] += countMatStride;
        };
    };

    
    time(&rawTime);
    P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Finished collapsing UMIs" <<endl;
};