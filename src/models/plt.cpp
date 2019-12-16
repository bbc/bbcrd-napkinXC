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
                           std::vector<std::vector<double>*>* binWeights,
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

        auto rSize = labels.size(r);
        auto rLabels = labels[r];

        // Check row
        for (int i = 0; i < rSize; ++i) {
            auto ni = tree->leaves.find(rLabels[i]);
            if (ni == tree->leaves.end()) {
                std::cerr << "Row: " << r << ", encountered example with label that does not exists in the tree!\n";
                continue;
            }
        }

        getNodesToUpdate(nPositive, nNegative, rLabels, rSize);
        addFeatures(binLabels, binFeatures, nPositive, nNegative, features[r]);

        nCount += nPositive.size() + nNegative.size();
        ++rCount;
    }
}

void PLT::getNodesToUpdate(std::unordered_set<TreeNode*>& nPositive, std::unordered_set<TreeNode*>& nNegative,
                           const int* rLabels, const int rSize){
    for (int i = 0; i < rSize; ++i) {
        TreeNode *n = tree->leaves[rLabels[i]];
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
    std::priority_queue<TreeNodeValue> nQueue;

    nQueue.push({tree->root, bases[tree->root->index]->predictProbability(features)});
    ++nCount;
    ++rCount;

    Prediction p = predictNextLabel(nQueue, features, args.threshold);
    while ((prediction.size() < args.topK || args.topK == 0) && p.label != -1) {
        prediction.push_back(p);
        p = predictNextLabel(nQueue, features, args.threshold);
    }
}

Prediction PLT::predictNextLabel(std::priority_queue<TreeNodeValue>& nQueue, Feature* features, double threshold) {
    while (!nQueue.empty()) {
        TreeNodeValue nVal = nQueue.top();
        nQueue.pop();

        if(!nVal.node->children.empty()){
            for(const auto& child : nVal.node->children)
                addToQueue(nQueue, child, nVal.value * bases[child->index]->predictProbability(features), threshold);
            nCount += nVal.node->children.size();
        }
        if(nVal.node->label >= 0)
            return {nVal.node->label, nVal.value};
    }

    return {-1, 0};
}

double PLT::predictForLabel(Label label, Feature* features, Args &args){
    TreeNode *n = tree->leaves[label];
    double value = bases[n->index]->predictProbability(features);
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

void BatchPLT::train(SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args& args, std::string output){

    // Create tree
    if(!tree) {
        tree = new Tree();
        tree->buildTreeStructure(labels, features, args);
    }
    m = tree->getNumberOfLeaves();

    std::cerr << "Training tree ...\n";

    // Check data
    assert(features.rows() == labels.rows());
    assert(tree->k >= labels.cols());

    // Examples selected for each node
    std::vector<std::vector<double>> binLabels(tree->t);
    std::vector<std::vector<Feature*>> binFeatures(tree->t);
    std::vector<std::vector<double>*>* binWeights = nullptr;
    if(type == hsm && args.hsmPickOneLabelWeighting){
        binWeights = new std::vector<std::vector<double>*>();
        binWeights->resize(tree->t);
        for(auto& p : *binWeights) p = new std::vector<double>();
    }

    assignDataPoints(binLabels, binFeatures, binWeights, labels, features, args);
    trainBases(joinPath(output, "weights.bin"), features.cols(), binLabels, binFeatures, binWeights, args);

    // Save tree
    tree->saveToFile(joinPath(output, "tree.bin"));

    // Save tree structure
    tree->saveTreeStructure(joinPath(output, "tree.txt"));
}