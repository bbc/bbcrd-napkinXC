/**
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

#include "hsm.h"
#include "threads.h"


HSM::HSM() {
    eCount = 0;
    pLen = 0;
    name = "HSM";
}

void HSM::assignDataPoints(std::vector<std::vector<double>>& binLabels, std::vector<std::vector<Feature*>>& binFeatures,
                              std::vector<std::vector<double>*>* binWeights, SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args &args){
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
        if(!args.hsmPickOneLabelWeighting && rSize != 1) {
            std::cerr << "Row " << r << ": encountered example with " << rSize << " labels! HSM is multi-class classifier, use PLT instead!\n";
            continue;
        }

        for (int i = 0; i < rSize; ++i) {
            auto ni = tree->leaves.find(rLabels[i]);
            if (ni == tree->leaves.end()) {
                std::cerr << "Row: " << r << ", encountered example with label that does not exists in the tree!\n";
                continue;
            }

            getNodesToUpdate(nPositive, nNegative, rLabels[i]);
            addFeatures(binLabels, binFeatures, nPositive, nNegative, features[r]);
            if(args.hsmPickOneLabelWeighting) {
                double w = 1.0 / rSize;
                for (const auto &n : nPositive) (*binWeights)[n->index]->push_back(w);
                for (const auto &n : nNegative) (*binWeights)[n->index]->push_back(w);
            }

            nCount += nPositive.size() + nNegative.size();
        }
        ++rCount;
    }
}

void HSM::getNodesToUpdate(std::unordered_set<TreeNode*>& nPositive, std::unordered_set<TreeNode*>& nNegative,
                           const int rLabel){

    std::vector<TreeNode*> path;

    TreeNode *n = tree->leaves[rLabel];
    path.push_back(n);
    while (n->parent){
        n = n->parent;
        path.push_back(n);
    }

    assert(path.size());
    assert(path.back() == tree->root);

    for(int i = path.size() - 1; i >= 0; --i){
        TreeNode *n = path[i], *p = n->parent;
        if(p == nullptr || p->children.size() == 1){
            nPositive.insert(n);
            eCount += 1;
        }
        else if(p->children.size() == 2){ // Binary node requires just 1 probability estimator
            TreeNode *c0 = n->parent->children[0], *c1 = n->parent->children[1];
            if(c0 == n) nPositive.insert(c0);
            else nNegative.insert(c0);
            nNegative.insert(c1);
            eCount += 1;
        }
        else if(p->children.size() > 2){ // Node with arity > 2 requires OVR estimator
            for(const auto& c : p->children){
                if(c == n) nPositive.insert(c);
                else nNegative.insert(c);
            }
            eCount += p->children.size();
        }
    }

    pLen += path.size();
}

Prediction HSM::predictNextLabel(std::priority_queue<TreeNodeValue>& nQueue, Feature* features, double threshold){

    while (!nQueue.empty()) {
        TreeNodeValue nVal = nQueue.top();
        nQueue.pop();

        if(!nVal.node->children.empty()){
            if(nVal.node->children.size() == 2) {
                double value = bases[nVal.node->children[0]->index]->predictProbability(features);
                addToQueue(nQueue, nVal.node->children[0], nVal.value * value, threshold);
                addToQueue(nQueue, nVal.node->children[1], nVal.value * (1.0 - value), threshold);
                ++eCount;
            }
            else {
                double sum = 0;
                std::vector<double> values;
                values.reserve(nVal.node->children.size());
                for (const auto &child : nVal.node->children) {
                    //values.emplace_back(bases[child->index]->predictProbability(features)); // Normalization
                    values.emplace_back(std::exp(bases[child->index]->predictValue(features))); // Softmax normalization
                    sum += values.back();
                }

                for(int i = 0; i < nVal.node->children.size(); ++i)
                    addToQueue(nQueue, nVal.node->children[i], nVal.value * values[i] / sum, threshold);

                eCount += nVal.node->children.size();
            }
        }
        if(nVal.node->label >= 0)
            return {nVal.node->label, nVal.value};
    }

    return {-1, 0};
}

double HSM::predictForLabel(Label label, Feature* features, Args &args){
    double value = 0;
    TreeNode *n = tree->leaves[label];
    while (n->parent){
        if(n->parent->children.size() == 2) {
            if(n == n->parent->children[0])
                value *= bases[n->children[0]->index]->predictProbability(features);
            else
                value *= 1.0 - bases[n->children[0]->index]->predictProbability(features);
        }
        else {
            double sum = 0;
            double tmpValue = 0;
            for (const auto &child : n->parent->children) {
                if(child == n) {
                    tmpValue = bases[child->index]->predictProbability(features);
                    sum += tmpValue;
                } else
                    sum += bases[child->index]->predictProbability(features);
            }
            value *= tmpValue / sum;
        }
        n = n->parent;
    }

    return value;
}

void HSM::printInfo(){
    std::cerr << "HSM additional stats:"
              << "\n  Mean path len: " << static_cast<double>(pLen) / rCount
              << "\n  Mean # estimators per data point: " << static_cast<double>(eCount) / rCount
              << "\n";
}
