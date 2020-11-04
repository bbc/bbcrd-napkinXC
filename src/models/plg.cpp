/**
 * Copyright (c) 2019 by Marek Wydmuch
 * All rights reserved.
 */

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <list>
#include <vector>

#include "plg.h"
#include "misc.h"
#include "threads.h"


PLG::PLG() {

}

PLG::~PLG() {
    for (auto b : bases) delete b;
}

void PLG::train(SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args& args, std::string output) {
    int layerCount = args.plgLayers;
    layerSize = args.plgLayerSize;

    Log(CERR) << "  Number of graph layers: " << layerCount << ", number of nodes per layer: " << layerSize << "\n";

    m = labels.cols();
    std::ofstream out(joinPath(output, "graph.bin"));
    out.write((char*)&m, sizeof(m));
    out.write((char*)&layerCount, sizeof(layerCount));
    out.write((char*)&layerSize, sizeof(layerSize));

    // Generate hashes and save them to file
    long seed = args.getSeed();
    std::default_random_engine rng(seed);
    std::uniform_int_distribution dist(1, layerSize);
    for(int i = 0; i < layerCount; ++i){
        unsigned int a = getFirstBiggerPrime(dist(rng));
        unsigned int b = getFirstBiggerPrime(dist(rng));
        unsigned int p = getFirstBiggerPrime(layerSize + dist(rng));

        out.write((char*)&a, sizeof(a));
        out.write((char*)&b, sizeof(b));
        out.write((char*)&p, sizeof(p));

        hashes.emplace_back(a, b, p);
    }

    out.close();

    int size = layerSize + (layerSize * layerSize) * (hashes.size() - 1);

    int rows = features.rows();
    int lCols = labels.cols();
    assert(rows == labels.rows());

    std::vector<std::vector<double>> binLabels(size);
    std::vector<std::vector<Feature*>> binFeatures(size);

    for (int r = 0; r < rows; ++r) {
        printProgress(r, rows);

        int rSize = labels.size(r);
        auto rLabels = labels[r];
        auto rFeatures = features[r];
        
        UnorderedSet<int> posEdges;
        UnorderedSet<int> posNodes;

        for (int i = 0; i < rSize; ++i) {
            int prevNode = 0;
            for (int j = 0; j < hashes.size(); ++j) {
                posNodes.insert(prevNode);
                int nextNode = nodeForLabel(rLabels[i], j);
                int edge = prevNode * layerSize + nextNode;
                posEdges.insert(prevNode * layerSize + nextNode);
                prevNode = 1 + j * layerSize + nextNode;
            }
        }

        for(const auto& e : posEdges) {
            binLabels[e].push_back(1.0);
            binFeatures[e].push_back(rFeatures);
        }

        for (const auto& n : posNodes) {
            for(int i = n * layerSize; i < (n + 1) * layerSize; ++i){
                if(!posEdges.count(i)){
                    binLabels[i].push_back(0.0);
                    binFeatures[i].push_back(rFeatures);
                }
            }
        }
    }

    trainBases(joinPath(output, "weights.bin"), features.cols(), binLabels, binFeatures, nullptr, args);
}

void PLG::predict(std::vector<Prediction>& prediction, Feature* features, Args& args) {
    // Brute force prediction
    prediction.reserve(m);
    for (int i = 0; i < m; ++i)
        prediction.emplace_back(i, 0.0);

    for (int i = 0; i < bases.size(); ++i) {
        double value = bases[i]->predictProbability(features);
        for (const auto &l : baseToLabels[i])
            prediction[l].value *= value;
    }

    std::nth_element(prediction.begin(), prediction.begin() + args.topK, prediction.end(), std::greater<Prediction>());
    prediction.resize(args.topK);
    prediction.shrink_to_fit();
    std::sort(prediction.begin(), prediction.end(), std::greater<Prediction>());

    // TODO: Faster prediction
    /*
    std::priority_queue<Prediction> nQueue;
    std::vector<double> basePredictions(bases.size());
    for (int i = 0; i < bases.size(); ++i)
        basePredictions[i] = bases[i]->predictProbability(features);

    //...
     */

}

double PLG::predictForLabel(Label label, Feature* features, Args& args) {
    double prob = 1;
    for (int i = 0; i < hashes.size(); ++i)
        prob *= bases[nodeForLabel(label, i)]->predictProbability(features);
    return prob;
}

void PLG::load(Args& args, std::string infile) {
    Log(CERR) << "Loading weights ...\n";
    bases = loadBases(joinPath(infile, "weights.bin"));

    Log(CERR) << "Loading hashes ...\n";
    std::ifstream in(joinPath(infile, "graph.bin"));
    int layerCount;
    unsigned int a, b, p;
    in.read((char*)&m, sizeof(m));
    in.read((char*)&layerCount, sizeof(layerCount));
    in.read((char*)&layerSize, sizeof(layerSize));
    for(int i = 0; i < layerCount; ++i){
        in.read((char*)&a, sizeof(a));
        in.read((char*)&b, sizeof(b));
        in.read((char*)&p, sizeof(p));
        hashes.emplace_back(a, b, p);
    }
    in.close();

    // This is needed for fast brute force prediction
    baseToLabels.resize(bases.size());
    for(int i = 0; i < m; ++i) {
        int prevNode = 0;
        for (int j = 0; j < hashes.size(); ++j){
            int nextNode = nodeForLabel(i, j);
            int edge = prevNode * layerSize + nextNode;
            baseToLabels[edge].push_back(i);
            prevNode = 1 + j * layerSize + nextNode;
        }
    }
}
