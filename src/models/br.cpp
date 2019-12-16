/**
 * Copyright (c) 2019 by Marek Wydmuch
 * All rights reserved.
 */

#include <cassert>
#include <algorithm>
#include <vector>
#include <list>
#include <cmath>
#include <climits>

#include "br.h"
#include "threads.h"


BR::BR(){
    type = br;
    name = "BR";
}

BR::~BR() {
    for(auto b : bases) delete b;
}

void BR::train(SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args& args, std::string output){
    // Check data
    int rows = features.rows();
    int lCols = labels.cols();
    assert(rows == labels.rows());

    std::ofstream out(joinPath(output, "weights.bin"));
    int size = lCols;
    out.write((char*) &size, sizeof(size));

    // Calculate required memory
    unsigned long long reqMem = lCols * (rows * sizeof(double) + sizeof(void*)) + labels.mem() + features.mem();

    int parts = 1;
    int range = lCols / parts + 1;

    assert(lCols < range * parts);
    std::vector<std::vector<double>> binLabels(range);
    for(int i = 0; i < binLabels.size(); ++i)
        binLabels[i].reserve(rows);

    for(int p = 0; p < parts; ++p){

        if(parts > 1)
            std::cerr << "Assigning labels for base estimators (" << p + 1 << "/" << parts << ") ...\n";
        else
            std::cerr << "Assigning labels for base estimators ...\n";

        int rStart = p * range;
        int rStop = (p + 1) * range;

        for(int r = 0; r < rows; ++r){
            printProgress(r, rows);

            int rSize = labels.size(r);
            auto rLabels = labels.row(r);

            if(type == ovr && rSize != 1) {
                std::cerr << "Row " << r << ": encountered example with " << rSize << " labels! OVR is multi-class classifier, use BR instead!\n";
                continue;
            }

            for(auto &l : binLabels) l.push_back(0.0);

            for (int i = 0; i < rSize; ++i)
                if(rLabels[i] >= rStart && rLabels[i] < rStop)
                    binLabels[rLabels[i] - rStart].back() = 1.0;
        }

        trainBasesWithSameFeatures(out, features.cols(), binLabels, features.allRows(), nullptr, args);

        for(auto &l : binLabels) l.clear();
    }

    out.close();
}

void BR::predict(std::vector<Prediction>& prediction, Feature* features, Args &args){
    for(int i = 0; i < bases.size(); ++i)
        prediction.push_back({i, bases[i]->predictProbability(features)});

    sort(prediction.rbegin(), prediction.rend());
    resizePrediction(prediction, args);
}

double BR::predictForLabel(Label label, Feature* features, Args &args){
    return bases[label]->predictProbability(features);
}

void BR::resizePrediction(std::vector<Prediction>& prediction, Args &args){
    if(args.topK > 0)
        prediction.resize(args.topK);

    if(args.threshold > 0){
        int i = 0;
        while(prediction[i++].value > args.threshold);
        prediction.resize(i - 1);
    }
}

void BR::load(Args &args, std::string infile){
    std::cerr << "Loading weights ...\n";
    bases = loadBases(joinPath(infile, "weights.bin"));
    m = bases.size();
}

void BR::printInfo(){
    std::cerr << name << " additional stats:"
              << "\n  Mean # estimators per data point: " << bases.size()
              << "\n";
}