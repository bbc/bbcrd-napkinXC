/**
 * Copyright (c) 2018 by Marek Wydmuch, Kalina Jasinska, Robert Istvan Busa-Fekete
 * Copyright (c) 2019 by Marek Wydmuch
 * All rights reserved.
 */

#include <cassert>
#include <unordered_set>
#include <algorithm>
#include <vector>
#include <list>
#include <cmath>
#include <climits>

#include "plt.h"


PLT::PLT(){
    tree = nullptr;
    nCount = 0;
    rCount = 0;
    type = plt;
    name = "PLT";
}

PLT::~PLT(){
    delete tree;
    for(auto b : bases) delete b;
}

void PLT::assignDataPoints(std::vector<std::vector<double>>& binLabels, std::vector<std::vector<Feature*>>& binFeatures,
                           SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args &args){

    std::cerr << "Assigning data points to nodes ...\n";

    // Positive and negative nodes
    std::unordered_set<TreeNode*> nPositive;
    std::unordered_set<TreeNode*> nNegative;

    // Gather examples for each node
    int rows = features.rows();
    for(int r = 0; r < rows; ++r){
        printProgress(r, rows);

        nPositive.clear();
        nNegative.clear();

        getNodesToUpdate(r, nPositive, nNegative, labels[r], labels.size(r));
        addFeatures(binLabels, binFeatures, nPositive, nNegative, features[r]);

        nCount += nPositive.size() + nNegative.size();
        ++rCount;
    }
}

void PLT::getNodesToUpdate(const int row, std::unordered_set<TreeNode*>& nPositive, std::unordered_set<TreeNode*>& nNegative,
                           const int* rLabels, const int rSize){
    for (int i = 0; i < rSize; ++i) {
        auto ni = tree->leaves.find(rLabels[i]);
        if(ni == tree->leaves.end()) {
            std::cerr << "Row: " << row << ", encountered example with label that does not exists in the tree!\n";
            continue;
        }
        TreeNode *n = ni->second;
        nPositive.insert(n);
        while (n->parent) {
            n = n->parent;
            nPositive.insert(n);
        }
    }

    if(!nPositive.count(tree->root)){
        nNegative.insert(tree->root);
        return;
    }

    std::queue<TreeNode*> nQueue; // Nodes queue
    nQueue.push(tree->root); // Push root

    while(!nQueue.empty()) {
        TreeNode* n = nQueue.front(); // Current node
        nQueue.pop();

        for(const auto& child : n->children) {
            if (nPositive.count(child)) nQueue.push(child);
            else nNegative.insert(child);
        }
    }
}

void PLT::addFeatures(std::vector<std::vector<double>>& binLabels, std::vector<std::vector<Feature*>>& binFeatures,
                      std::unordered_set<TreeNode*>& nPositive, std::unordered_set<TreeNode*>& nNegative,
                      Feature* features){
    for (const auto& n : nPositive){
        binLabels[n->index].push_back(1.0);
        binFeatures[n->index].push_back(features);
    }

    for (const auto& n : nNegative){
        binLabels[n->index].push_back(0.0);
        binFeatures[n->index].push_back(features);
    }
}

void PLT::predict(std::vector<Prediction>& prediction, Feature* features, Args &args){
    predictTopK(prediction, features, args.topK);
}

void PLT::predictTopK(std::vector<Prediction>& prediction, Feature* features, int k){
    std::priority_queue<TreeNodeValue> nQueue;

    // Note: loss prediction gets worse results for tree with higher arity then 2
    double val = bases[tree->root->index]->predictProbability(features);
    //double val = -bases[tree->root->index]->predictLoss(features);
    nQueue.push({tree->root, val});
    ++nCount;
    ++rCount;

    while (prediction.size() < k && !nQueue.empty()) prediction.push_back(predictNext(nQueue, features));
}

Prediction PLT::predictNext(std::priority_queue<TreeNodeValue>& nQueue, Feature* features) {
    while (!nQueue.empty()) {
        TreeNodeValue nVal = nQueue.top();
        nQueue.pop();

        if(nVal.node->children.size()){
            for(const auto& child : nVal.node->children){
                double value = nVal.value * bases[child->index]->predictProbability(features); // When using probability
                //double value = nVal.value - bases[child->index]->predictLoss(features); // When using loss
                nQueue.push({child, value});
            }
            nCount += nVal.node->children.size();
        }
        if(nVal.node->label >= 0)
            return {nVal.node->label, nVal.value};
    }

    return {-1, 0};
}

double PLT::predictForLabel(Label label, Feature* features, Args &args){
    double value = 0;
    TreeNode *n = tree->leaves[label];
    value *= bases[n->index]->predictProbability(features);
    while (n->parent){
        n = n->parent;
        value *= bases[n->index]->predictProbability(features);
    }
    return value;
}

void PLT::load(Args &args, std::string infile){
    std::cerr << "Loading " << name << " model ...\n";

    tree = new Tree();
    tree->loadFromFile(joinPath(infile, "tree.bin"));
    bases = loadBases(joinPath(infile, "weights.bin"));
    assert(bases.size() == tree->nodes.size());
    m = tree->getNumberOfLeaves();
}

void PLT::printInfo(){
    std::cerr << "PLT additional stats:"
              << "\n  Mean # nodes per data point: " << static_cast<double>(nCount) / rCount
              << "\n";
}
