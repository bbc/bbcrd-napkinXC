/**
 * Copyright (c) 2018 by Marek Wydmuch, Kalina Jasinska, Robert Istvan Busa-Fekete
 * All rights reserved.
 */

#include <cassert>
#include <unordered_set>
#include <algorithm>
#include <vector>
#include <queue>
#include <list>
#include <chrono>
#include <random>
#include <cmath>
#include <climits>

#include "pltree.h"
#include "utils.h"
#include "threads.h"

PLTree::PLTree(){}

PLTree::~PLTree() {
    for(size_t i = 0; i < tree.size(); ++i)
        delete tree[i];
}

Base* nodeTrainThread(int i, int n, std::vector<double>& binLabels, std::vector<Feature*>& binFeatures, Args& args){
    Base* base = new Base();
    base->train(n, binLabels, binFeatures, args);
    return base;
}

std::vector<std::vector<int>> splitLabels(std::vector<int> labels, const Args &args){
    std::vector<std::vector<int>> labelSplits;
    int partSize = ceil(float(labels.size()) / args.arity);
    std::vector<int>::const_iterator partBegin = labels.cbegin();
    while(partBegin < labels.cend()){
        std::vector<int> split =  std::vector<int>(partBegin, min(partBegin + partSize, labels.cend()));
        std::sort(split.begin(), split.end());;
        labelSplits.push_back(split);
        partBegin += partSize;
    }
    return labelSplits;
}

std::vector<struct JobResult> processJob(int index, const std::vector<int>& jobInstances,
                                         const std::vector<int>& jobLabels, std::ofstream& out,
                                         SRMatrix<Label>& labels, SRMatrix<Feature>& features,
                                         Args& args){
    //TODO add to params if trained multiple times
    int maxIter = 1000;
    int iter = 0;
    bool converged = false;
    std::vector<struct JobResult> results;

    std::vector<std::vector<int>> childPositiveInstances;
    std::vector<std::vector<int>> childLabels;
    std::vector<std::vector<double>> binLabelsChild;
    std::vector<Feature*> binFeatures;
    std::vector<Base*> childBases(args.arity);

//    for(std::vector<int>::const_iterator i = jobInstances.cbegin(); i != jobInstances.cend(); i++ ){
    for(int i = 0; i < jobInstances.size(); i++ ){
        binFeatures.push_back(features.data()[jobInstances[i]]);
    }

    int nodeArity;

    while(true) {
        //split labels
        childLabels = splitLabels(jobLabels, args);
        nodeArity = int(childLabels.size());
        assert(args.arity >= nodeArity);

        for(int i = 0; i <  nodeArity; i++){
            std::vector<double> cBinLabels;
            std::vector<int> cInstaces;

            for(std::vector<int>::const_iterator n = jobInstances.cbegin(); n != jobInstances.cend(); n++ ) {
                double binLabel = 0.0;
                for (int j = 0; j < labels.sizes()[*n]; ++j) {
                    auto label = labels.data()[*n][j];
                    //TODO use unordered_set instead
                    if (!binLabel and std::binary_search(childLabels[i].begin(), childLabels[i].end(), label)) {
                        binLabel = 1.0;
                        cInstaces.push_back(*n);
                        break;
                    }
                }
                cBinLabels.push_back(binLabel);
            }
            binLabelsChild.push_back(cBinLabels);
            childPositiveInstances.push_back(cInstaces);
        }

        //TODO remove old models if trained multiple times
        for(int i = 0; i <  nodeArity; i++){
            Base *base = new Base();
            base->train(features.cols(), binLabelsChild[i], binFeatures, args);
            childBases[i] = base;
        }

        // evaluate if convergence criteria are meet
        converged = true;

        if(converged or (iter++ > maxIter)) {
            break;
        }
    }

    for(int i = 0; i <  nodeArity; i++) {
        struct JobResult result{
                .base = childBases[i],
                .parent = index,
                .instances = childPositiveInstances[i],
                .labels = childLabels[i]
        };
        results.push_back(result);
    }
    return results;
}

struct JobResult PLTree::trainRoot(SRMatrix<Label> &labels, SRMatrix<Feature> &features, Args &args){
    std::vector<double> binLabels;
    std::vector<Feature*> binFeatures;
    std::vector<int> rootPositiveIndices;

    for(int r = 0; r < labels.rows(); ++r){
        binFeatures.push_back(features.data()[r]);
        if (labels.sizes()[r] > 0) {
            binLabels.push_back(1.0);
            rootPositiveIndices.push_back(r);
        } else {
            binLabels.push_back(0.0);
        }
    }

    Base *base = new Base();
    base->train(features.cols(), binLabels, binFeatures, args);

    JobResult result;
    result.parent = -1;
    result.instances = rootPositiveIndices;
    result.base = base;
    return result;
}

void PLTree::addModelToTree(Base *model, int parent, std::vector<int> &labels, std::vector<int> &instances,
                            std::ofstream &out, Args &args, std::vector<NodeJob> &nextLevelJobs){
    TreeNode *node = new TreeNode();
    tree.push_back(node);
    node->index = tree.size() - 1;
    model->save(out, args);
    delete model;


    if(parent == -1){ //ROOT
        node->parent = nullptr;
        treeRoot = tree[0];
        if(labels.size() > 0){
            node->label = -1;
            if(labels.size() > 1){
                nextLevelJobs.push_back(NodeJob{.parent = 0, .labels = labels, .instances = instances});
            }
        }
    } else {
        tree[parent]->children.push_back(node);
        node->parent = tree[parent];
        assert(tree[parent]->label == -1);

        if(labels.size() > 1) {
            nextLevelJobs.push_back(NodeJob{.parent = node->index, .labels = labels, .instances = instances });
            node->label = -1;
        } else {
            node->label = labels[0];
            treeLeaves[node->label] = node;
        }
    }
}


void PLTree::trainTopDown(SRMatrix<Label> &labels, SRMatrix<Feature> &features, Args &args){

    std::vector<struct NodeJob> jobs;
    std::vector<struct NodeJob> nextLevelJobs;

    std::ofstream out(args.model + "/weights.bin");

    struct JobResult rootResult = trainRoot(labels, features, args);
    std::vector<int> allLabels(labels.cols());//TODO: determine the number/list of unique labels/ in some correct way
    std::iota(allLabels.begin(), allLabels.end(), 0); // TODO: labels start from 0?
    addModelToTree(rootResult.base, -1, allLabels, rootResult.instances, out, args, jobs);
    //TODO remove copying of vectors
    if(args.threads > 1){
        // Run learning in parallel
        ThreadPool tPool(args.threads);
        while(jobs.size() != 0) {
            std::vector<std::future<std::vector<JobResult>>> levelResults;
            for (auto &job : jobs) {
                levelResults.emplace_back(
                        tPool.enqueue(processJob, job.parent, std::cref(job.instances), std::cref(job.labels),
                                      std::ref(out), std::ref(labels), std::ref(features), std::ref(args)));
            }

            for (int i = 0; i < levelResults.size(); ++i) {
                std::vector<JobResult> results;
                results = levelResults[i].get();

                for (auto result : results) {
                    addModelToTree(result.base, result.parent, result.labels, result.instances, out, args,
                                   nextLevelJobs);
                }
            }

            jobs = nextLevelJobs;
            nextLevelJobs.clear();
        }

    } else {
        while(jobs.size() != 0){
            nextLevelJobs.clear();
            for(auto job : jobs){
                std::vector<JobResult> results = processJob(job.parent, job.instances, job.labels, out, labels, features, args);
                for(auto result : results){
                    addModelToTree(result.base, result.parent, result.labels, result.instances,out, args, nextLevelJobs);
                }
                printProgress(job.parent, labels.cols());
            }
            jobs = nextLevelJobs;
            nextLevelJobs.clear();
        }
    }
    out.close();

    std::cerr<<std::endl<<"Training finished."<<std::endl;

    t = tree.size();
    k = treeLeaves.size();

    assert(k >= labels.cols());

    save(args.model + "/tree.bin");
    args.save(args.model + "/args.bin");
}

void PLTree::train(SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args& args){
    rng.seed(args.seed);

    if(args.treeType == topDown){
        // Top down building and training
        trainTopDown(labels, features, args);
    } else {

        // Create a tree structure
        if (!args.tree.empty()) loadTreeStructure(args.tree);
        else if (args.treeType == completeInOrder)
            buildCompleteTree(labels.cols(), args.arity, false);
        else if (args.treeType == completeRandom)
            buildCompleteTree(labels.cols(), args.arity, true);
        else if (args.treeType == balancedInOrder)
            buildBalancedTree(labels.cols(), args.arity, false);
        else if (args.treeType == balancedRandom)
            buildBalancedTree(labels.cols(), args.arity, true);
        else if (args.treeType == huffman)
            buildHuffmanPLTree(labels, args);
        else if (args.treeType == hierarchicalKMeans) {
            SRMatrix<Feature> labelsFeatures;
            buildLabelsFeaturesMatrix(labelsFeatures, labels, features);
            buildKMeansTree(labelsFeatures, args);
        }
        else if(args.treeType == kMeansWithProjection)
            balancedKMeansWithRandomProjection(labels, features, args);
        else {
            std::cerr << "Unknown tree type\n";
            exit(0);
        }

        // Train the tree structure
        trainTreeStructure(labels, features, args);
    }
}

void PLTree::trainTreeStructure(SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args& args){
    std::cerr << "Training tree ...\n";

    // For stats
    long nCount = 0, yCount = 0;

    // Check data
    int rows = features.rows();
    assert(rows == labels.rows());
    assert(k >= labels.cols());

    // Check tree
    assert(k == treeLeaves.size());
    assert(t == tree.size());

    // Examples selected for each node
    std::vector<std::vector<double>> binLabels(t);
    std::vector<std::vector<Feature*>> binFeatures(t);

    // Positive and negative nodes
    std::unordered_set<TreeNode*> nPositive;
    std::unordered_set<TreeNode*> nNegative;

    std::cerr << "Assigning points to nodes ...\n";

    // Gather examples for each node
    for(int r = 0; r < rows; ++r){
        printProgress(r, rows);

        nPositive.clear();
        nNegative.clear();

        int rSize = labels.size(r);
        auto rLabels = labels.row(r);

        if (rSize > 0){
            for (int i = 0; i < rSize; ++i) {
                TreeNode *n = treeLeaves[rLabels[i]];
                nPositive.insert(n);
                while (n->parent) {
                    n = n->parent;
                    nPositive.insert(n);
                }
            }

            std::queue<TreeNode*> nQueue; // Nodes queue
            nQueue.push(treeRoot); // Push root

            while(!nQueue.empty()) {
                TreeNode* n = nQueue.front(); // Current node
                nQueue.pop();

                for(const auto& child : n->children) {
                    if (nPositive.count(child)) nQueue.push(child);
                    else nNegative.insert(child);
                }
            }
        } else nNegative.insert(treeRoot);

        for (const auto& n : nPositive){
            binLabels[n->index].push_back(1.0);
            binFeatures[n->index].push_back(features.row(r));
        }

        for (const auto& n : nNegative){
            binLabels[n->index].push_back(0.0);
            binFeatures[n->index].push_back(features.row(r));
        }

        nCount += nPositive.size() + nNegative.size();
        yCount += rSize;
    }

    std::cerr << "Starting training in " << args.threads << " threads ...\n";

    std::ofstream out(args.model + "/weights.bin");
    if(args.threads > 1){
        // Run learning in parallel
        ThreadPool tPool(args.threads);
        std::vector<std::future<Base*>> results;

        for(const auto& n : tree)
            results.emplace_back(tPool.enqueue(nodeTrainThread, n->index, features.cols(), std::ref(binLabels[n->index]),
                                               std::ref(binFeatures[n->index]), std::ref(args)));

        // Saving in the main thread
        for(int i = 0; i < results.size(); ++i) {
            printProgress(i, results.size());
            Base* base = results[i].get();
            base->save(out, args);
            delete base;
        }
    } else {
        for(int i = 0; i < tree.size(); ++i){
            printProgress(i, tree.size());
            Base base;
            base.train(features.cols(), binLabels[tree[i]->index], binFeatures[tree[i]->index], args);
            base.save(out, args);
        }
    }
    out.close();

    std::cerr << "  Points count: " << rows
                << "\n  Nodes per point: " << static_cast<double>(nCount) / rows
                << "\n  Labels per point: " << static_cast<double>(yCount) / rows
                << "\n";

    // Save data
    save(args.model + "/tree.bin");
    args.save(args.model + "/args.bin");
    std::cerr << "All done\n";
}

void PLTree::predict(std::vector<TreeNodeValue>& prediction, Feature* features, std::vector<Base*>& bases, int k){
    std::priority_queue<TreeNodeValue> nQueue;

    // Note: loss prediction gets worse results for tree with higher arity then 2

    double val = bases[treeRoot->index]->predictProbability(features);
    //double val = -bases[treeRoot->index]->predictLoss(features);
    nQueue.push({treeRoot, val});

    while (!nQueue.empty()) {
        TreeNodeValue nVal = nQueue.top(); // Current node
        nQueue.pop();

        if(nVal.node->label >= 0){
            prediction.push_back({nVal.node, nVal.value}); // When using probability
            //prediction.push_back({nVal.node, exp(nVal.value)}); // When using loss
            if (prediction.size() >= k)
                break;
        } else {
            for(const auto& child : nVal.node->children){
                val = nVal.value * bases[child->index]->predictProbability(features); // When using probability
                //val = nVal.value - bases[child->index]->predictLoss(features); // When using loss
                nQueue.push({child, val});
            }
        }
    }
}

std::mutex testMutex;
int pointTestThread(PLTree* tree, Label* labels, Feature* features, std::vector<Base*>& bases,
    int k, std::vector<int>& correctAt){

    std::vector<TreeNodeValue> prediction;
    tree->predict(prediction, features, bases, k);

    testMutex.lock();
    for (int i = 0; i < k; ++i){
        int l = -1;
        while(labels[++l] > -1)
            if (prediction[i].node->label == labels[l]){
                ++correctAt[i];
                break;
            }
    }
    testMutex.unlock();

    return 0;
}

int batchTestThread(PLTree* tree, SRMatrix<Label>& labels, SRMatrix<Feature>& features,
    std::vector<Base*>& bases, int topK, int startRow, int stopRow, std::vector<int>& correctAt){

    std::vector<int> localCorrectAt (topK);
    for(int r = startRow; r < stopRow; ++r){
        std::vector<TreeNodeValue> prediction;
        tree->predict(prediction, features.row(r), bases, topK);

        for (int i = 0; i < topK; ++i)
            for (int j = 0; j < labels.size(r); ++j)
                if (prediction[i].node->label == labels.row(r)[j]){
                    ++localCorrectAt[i];
                    break;
                }
    }

    testMutex.lock();
    for (int i = 0; i < topK; ++i)
        correctAt[i] += localCorrectAt[i];
    testMutex.unlock();

    return 0;
}

void PLTree::test(SRMatrix<Label>& labels, SRMatrix<Feature>& features, std::vector<Base*>& bases, Args& args) {
    std::cerr << "Starting testing ...\n";

    std::vector<int> correctAt(args.topK);
    int rows = features.rows();
    assert(rows == labels.rows());

    if(args.threads > 1){
        // Run prediction in parallel

        // Pool
        ThreadPool tPool(args.threads);
        std::vector<std::future<int>> results;

        for(int r = 0; r < rows; ++r)
            results.emplace_back(tPool.enqueue(pointTestThread, this, labels.data()[r], features.data()[r],
                                               std::ref(bases), args.topK, std::ref(correctAt)));

        for(int i = 0; i < results.size(); ++i) {
            printProgress(i, results.size());
            results[i].get();
        }

        // Batches
        /*
        ThreadSet tSet;
        int tRows = ceil(static_cast<double>(rows)/args.threads);
        for(int t = 0; t < args.threads; ++t)
            tSet.add(batchTestThread, this, std::ref(labels), std::ref(features), std::ref(bases),
                args.topK, t * tRows, std::min((t + 1) * tRows, labels.rows()), std::ref(precisionAt));
        tSet.joinAll();
        */

    } else {
        std::vector<TreeNodeValue> prediction;
        for(int r = 0; r < rows; ++r){
            prediction.clear();
            predict(prediction, features.data()[r], bases, args.topK);

            for (int i = 0; i < args.topK; ++i)
                for (int j = 0; j < labels.sizes()[r]; ++j)
                    if (prediction[i].node->label == labels.data()[r][j]){
                        ++correctAt[i];
                        break;
                    }
            printProgress(r, rows);
        }
    }

    double precisionAt = 0;
    for (int i = 0; i < args.topK; ++i) {
        precisionAt += correctAt[i];
        std::cerr << "P@" << i + 1 << ": " << precisionAt / (rows * (i + 1)) << "\n";
    }
    std::cerr << "All done\n";
}

void PLTree::loadTreeStructure(std::string file){
    std::cerr << "Loading PLTree structure from: " << file << "...\n";

    std::ifstream in(file);
    in >> k >> t;

    treeRoot = createTreeNode();
    for (int i = 1; i < t; ++i) createTreeNode();

    for (auto i = 0; i < t - 1; ++i) {
        int parent, child, label;
        in >> parent >> child >> label;

        if(parent == -1){
            treeRoot = tree[child];
            --i;
            continue;
        }

        TreeNode *parentN = tree[parent];
        TreeNode *childN = tree[child];
        parentN->children.push_back(childN);
        childN->parent = parentN;

        if(label >= 0){
            assert(treeLeaves.count(label) == 0);
            assert(label < k);
            childN->label = label;
            treeLeaves[childN->label] = childN;
        }
    }
    in.close();

    assert(tree.size() == t);
    assert(treeLeaves.size() == k);
    std::cout << "  Nodes: " << tree.size() << ", leaves: " << treeLeaves.size() << "\n";
}

TreeNodePartition kMeansThread(TreeNodePartition nPart, SRMatrix<Feature>& labelsFeatures, Args& args, int seed){
    kMeans(nPart.partition, labelsFeatures, args.arity, args.kMeansEps, args.kMeansBalanced, seed);
    return nPart;
}

// TODO: Make it parallel
void PLTree::buildLabelsFeaturesMatrix(SRMatrix<Feature>& labelsFeatures, SRMatrix<Label>& labels, SRMatrix<Feature>& features){
    std::cerr << "Building labels' features matrix ...\n";

    std::vector<std::unordered_map<int, double>> tmpLabelsFeatures(labels.cols());

    int rows = features.rows();
    assert(rows == labels.rows());

    for(int r = 0; r < rows; ++r){
        printProgress(r, rows);
        int rFeaturesSize = features.sizes()[r];
        int rLabelsSize = labels.sizes()[r];
        auto rFeatures = features.data()[r];
        auto rLabels = labels.data()[r];

        for (int i = 0; i < rFeaturesSize; ++i){
            for (int j = 0; j < rLabelsSize; ++j){
                if (!tmpLabelsFeatures[rLabels[j]].count(rFeatures[i].index))
                    tmpLabelsFeatures[rLabels[j]][rFeatures[i].index] = 0;
                tmpLabelsFeatures[rLabels[j]][rFeatures[i].index] += rFeatures[i].value;
            }
        }
    }

    for(int l = 0; l < labels.cols(); ++l){
        std::vector<Feature> labelFeatures;
        for(const auto& f : tmpLabelsFeatures[l])
            labelFeatures.push_back({f.first, f.second});
        std::sort(labelFeatures.begin(), labelFeatures.end());
        unitNorm(labelFeatures);
        labelsFeatures.appendRow(labelFeatures);
    }
}

void PLTree::buildKMeansTree(SRMatrix<Feature>& labelsFeatures, Args& args){
    std::cerr << "Hierarchical K-Means clustering in " << args.threads << " threads ...\n";

    treeRoot = createTreeNode();
    k = labelsFeatures.rows();

    std::uniform_int_distribution<int> kMeansSeeder(0, INT_MAX);

    auto partition = new std::vector<Assignation>(k);
    for(int i = 0; i < k; ++i) (*partition)[i].index = i;

    if(args.threads > 1){
        // Run clustering in parallel
        ThreadPool tPool(args.threads);
        std::vector<std::future<TreeNodePartition>> results;

        TreeNodePartition rootPart = {treeRoot, partition};
        results.emplace_back(tPool.enqueue(kMeansThread, rootPart, std::ref(labelsFeatures),
                                           std::ref(args), kMeansSeeder(rng)));

        for(int r = 0; r < results.size(); ++r) {
            // Enqueuing new clustering tasks in the main thread ensures determinism
            TreeNodePartition nPart = results[r].get();

            // This needs to be done this way in case of imbalanced K-Means
            auto partitions = new std::vector<Assignation>* [args.arity];
            for (int i = 0; i < args.arity; ++i) partitions[i] = new std::vector<Assignation>();
            for (auto a : *nPart.partition) partitions[a.value]->push_back({a.index, 0});

            for (int i = 0; i < args.arity; ++i) {
                TreeNode *n = createTreeNode(nPart.node);

                if(partitions[i]->size() <= args.maxLeaves) {
                    for (auto& a : *partitions[i]) createTreeNode(n, a.index);
                    delete partitions[i];
                } else {
                    TreeNodePartition childPart = {n, partitions[i]};
                    results.emplace_back(tPool.enqueue(kMeansThread, childPart, std::ref(labelsFeatures),
                                                       std::ref(args), kMeansSeeder(rng)));
                }
            }

            delete nPart.partition;
        }
    } else {
        std::queue<TreeNodePartition> nQueue;
        nQueue.push({treeRoot, partition});

        while (!nQueue.empty()) {
            TreeNodePartition nPart = nQueue.front(); // Current node
            nQueue.pop();

            if (nPart.partition->size() > args.maxLeaves) {
                kMeans(nPart.partition, labelsFeatures, args.arity, args.kMeansEps, args.kMeansBalanced, kMeansSeeder(rng));
                auto partitions = new std::vector<Assignation>* [args.arity];
                for (int i = 0; i < args.arity; ++i) partitions[i] = new std::vector<Assignation>();
                for (auto a : *nPart.partition) partitions[a.value]->push_back({a.index, 0});

                // Create children
                for (int i = 0; i < args.arity; ++i) {
                    TreeNode *n = createTreeNode(nPart.node);
                    nQueue.push({n, partitions[i]});
                }
            } else
                for (auto& a : *nPart.partition) createTreeNode(nPart.node, a.index);

            delete nPart.partition;
        }
    }

    t = tree.size();
    assert(k == treeLeaves.size());
    std::cerr << "  Nodes: " << tree.size() << ", leaves: " << treeLeaves.size() << "\n";
}

void PLTree::balancedKMeansWithRandomProjection(SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args &args) {

    int k = labels.cols();
    int n = features.rows();
    int dim = features.cols();

    std::cerr << "  Compute label to indices ...\n";
    std::vector<std::vector<int>> labelToIndices(k);
    for (int r = 0; r < n; ++r) {
        int rSize = labels.size(r);
        auto rLabels = labels.row(r);
        for (int i = 0; i < rSize; ++i) labelToIndices[rLabels[i]].push_back(r);
    }

    // Apply random projection
    std::vector<std::vector<double>> randomMatrix;
    generateRandomProjection(randomMatrix, args.projectDim, dim);
    SRMatrix<Feature> labelsFeatures(k, args.projectDim);
    projectLabelsRepresentation(labelsFeatures, randomMatrix, labelToIndices, features, args);

    // Build tree using hierarchical K-Means
    buildKMeansTree(labelsFeatures, args);
}

void PLTree::projectLabelsRepresentation(SRMatrix<Feature>& labelsFeatures, std::vector<std::vector<double>>& randomMatrix,
                                        std::vector<std::vector<int>>& labelToIndices, SRMatrix<Feature>& features, Args &args){

    int labels = labelToIndices.size();

    double scale = 1.0/sqrt(static_cast<double>(features.cols()));
    std::normal_distribution<double> distribution(0.0, scale);

    std::cerr << "  Compute projected values ...\n";
    for(int i=0; i < labels; i++ ){
        printProgress(i, labels);
        int currentLabel = i;
        auto labelVector = labelsFeatures.row(i);

        if (labelToIndices[currentLabel].size() > 0) {
            for (int j = 0; j < labelToIndices[currentLabel].size(); j++) {

                int currentDataPoint = labelToIndices[currentLabel][j];
                auto rFeatures = features.row(currentDataPoint);
                int rFeaturesSize = features.size(currentDataPoint);

                for (int l = 0; l < args.projectDim; l++)
                    for (int k = 0; k < rFeaturesSize; k++)
                        labelVector[l].value += rFeatures[k].value * randomMatrix[l][rFeatures[k].index];
                for (int l = 0; l < args.projectDim; l++)
                    labelVector[l].value /= labelToIndices[currentLabel].size();

                // Print row from labels' features matrix
                /*
                for (int l = 0; l < args.projectDim; l++) {
                    std::cout << labelsFeatures.data()[currentLabel][l].value << " ";
                std::cout << "\n";
                */
            }
        } else {
            for (int l = 0; l < args.projectDim; l++)
                labelVector[l].value = distribution(rng);
        }
    }
}

void PLTree::generateRandomProjection(std::vector<std::vector<double>>& randomMatrix, int projectDim, int dim){
    double scale = 1.0 / std::sqrt(static_cast<double>(dim));
    std::normal_distribution<double> distribution(0.0, scale);

    randomMatrix.resize(projectDim);
    for(int i = 0; i < projectDim; ++i) { // args.projectDim
        randomMatrix[i].resize(dim);
        for (int j = 0; j < dim; ++j) // dim
            randomMatrix[i][j] = distribution(rng);
    }
}


void PLTree::buildHuffmanPLTree(SRMatrix<Label>& labels, Args &args){
    std::cout << "Building PLT with Huffman tree ...\n";

    k = labels.cols();

    std::cout << "  Compute labels frequencies ...\n";
    std::vector<int64_t> freq(k);
    for(int i=0; i<k; i++) freq[i]=0;
    for(int r=0; r<labels.rows(); r++) {
        int rSize = labels.sizes()[r];
        auto rLabels = labels.data()[r];

        for (int i = 0; i < rSize; ++i) freq[rLabels[i]]++;
    }

    std::priority_queue<FreqTuple*, std::vector<FreqTuple*>, DereferenceCompareNode> freqheap;
    for(int i=0; i<k; i++) {
        TreeNode* n = new TreeNode();
        n->index = tree.size();
        n->label = i;
        treeLeaves[n->label] = n;
        tree.push_back(n);

        FreqTuple* f = new FreqTuple(freq[i], n);
        freqheap.push(f);

        //std::cout << "Leaf: " << n->label << ", Node: " << n->n << ", Freq: " << freq[i] << "\n";
    }

    while (1) {
        std::vector<FreqTuple*> toMerge;
        for(int a = 0; a < args.arity; ++a){
            FreqTuple* tmp = freqheap.top();
            freqheap.pop();
            toMerge.push_back(tmp);

            if (freqheap.empty()) break;
        }

        TreeNode* parent = new TreeNode();
        parent->index = tree.size();
        parent->label = -1;

        int64_t aggregatedFrequency = 0;
        for( FreqTuple* e : toMerge){
            e->node->parent = parent;
            parent->children.push_back(e->node);
            aggregatedFrequency += e->getFrequency();
        }

        tree.push_back(parent);

        if (freqheap.empty()) {
            treeRoot = parent;
            treeRoot->parent = nullptr;
            break;
        }

        FreqTuple* tup = new FreqTuple(aggregatedFrequency,parent);
        freqheap.push(tup);
    }

    t = tree.size();  // size of the tree
    std::cout << "  Nodes: " << tree.size() << ", leaves: " << treeLeaves.size() << ", arity: " << args.arity << "\n";
}


void PLTree::buildBalancedTree(int labelCount, int arity, bool randomizeTree) {
    std::cerr << "Building balanced PLTree ...\n";

    std::default_random_engine rng(time(0));

    std::vector<int> labelsOrder;
    for (auto i = 0; i < labelCount; ++i) labelsOrder.push_back(i);
    if (randomizeTree){
        std::random_shuffle(labelsOrder.begin(), labelsOrder.end());
    }

    std::queue<std::tuple<std::vector<int>::const_iterator, std::vector<int>::const_iterator, int>> begin_end_parent;
    begin_end_parent.push(std::make_tuple(labelsOrder.cbegin(), labelsOrder.cend(), -1));
    int c;

    while(!begin_end_parent.empty()){
        auto bep = begin_end_parent.front();
        begin_end_parent.pop();
        auto begin = std::get<0>(bep);
        auto endd = std::get<1>(bep);
        auto parent = std::get<2>(bep);

        if(begin + 1 == endd){
            TreeNode *n = new TreeNode();
            n->index = tree.size();
            tree.push_back(n);
            n->label = *begin;
            treeLeaves[n->label] = n;
            if(parent != -1){
                tree[parent]->children.push_back(n);
                n->parent = tree[parent];
            }

        } else {
            TreeNode *n = new TreeNode();
            n->index = tree.size();
            tree.push_back(n);
            n->label = -1;
            if(parent != -1){
                tree[parent]->children.push_back(n);
                n->parent = tree[parent];
            }

            int partSize = ceil(float(endd - begin)/arity);
            std::vector<int>::const_iterator partBegin = begin;
            c = 0;
            while (partBegin < endd){
                assert(c++ < arity);
                begin_end_parent.push(std::make_tuple(partBegin, min(partBegin + partSize, endd), n->index));
                partBegin += partSize;
            }
        }
    }

    treeRoot = tree[0];
    treeRoot->parent = nullptr;

    k = treeLeaves.size();
    t = tree.size();

    std::cerr << "  Nodes: " << tree.size() << ", leaves: " << treeLeaves.size() << ", arity: " << arity << "\n";
}

/*
void PLTree::buildTreeTopDown(SRMatrix<Label>& labels, SRMatrix<Feature>& features, Args &args){
    int n = features.rows(); // number of instances
    std::vector<int> active(0), left(0), right(0);

    for(int i=0; i < n; i++ ) active.push_back(i);

}

void PLTree::cut(SRMatrix<Label>& labels, SRMatrix<Feature>& features, std::vector<int>& active, std::vector<int>& left, std::vector<int>& right, Args &args){

}
*/

void PLTree::buildCompleteTree(int labelCount, int arity, bool randomizeOrder) {
    std::cerr << "Building complete PLTree ...\n";

    k = labelCount;
    t = static_cast<int>(ceil(static_cast<double>(arity * k - 1) / (arity - 1)));

    int ti = t - k;

    std::vector<int> labelsOrder;
    if (randomizeOrder){
        labelsOrder.resize(k);
        for (auto i = 0; i < k; ++i) labelsOrder[i] = i;
        std::shuffle(labelsOrder.begin(), labelsOrder.end(), rng);
    }

    treeRoot = createTreeNode();
    for(size_t i = 1; i < t; ++i){
        int label = -1;
        TreeNode *parent = nullptr;

        if(i >= ti){
            if(randomizeOrder) label = labelsOrder[i - ti];
            else label = i - ti;
        }

        parent = tree[static_cast<int>(floor(static_cast<double>(i - 1) / arity))];
        createTreeNode(parent, label);
    }

    std::cerr << "  Nodes: " << tree.size() << ", leaves: " << treeLeaves.size() << ", arity: " << arity << "\n";
}

TreeNode* PLTree::createTreeNode(TreeNode* parent, int label){
    TreeNode* n = new TreeNode();
    n->index = tree.size();
    n->label = label;
    n->parent = parent;
    if(label >= 0) treeLeaves[n->label] = n;
    if(parent != nullptr) parent->children.push_back(n);
    tree.push_back(n);

    return n;
}

void PLTree::save(std::string outfile){
    std::ofstream out(outfile);
    save(out);
    out.close();
}

void PLTree::save(std::ostream& out){
    std::cerr << "Saving PLTree model ...\n";

    out.write((char*) &k, sizeof(k));

    t = tree.size();
    out.write((char*) &t, sizeof(t));
    for(size_t i = 0; i < t; ++i) {
        TreeNode *n = tree[i];
        out.write((char*) &n->index, sizeof(n->index));
        out.write((char*) &n->label, sizeof(n->label));
    }

    int rootN = treeRoot->index;
    out.write((char*) &rootN, sizeof(rootN));

    for(size_t i = 0; i < t; ++i) {
        TreeNode *n = tree[i];

        int parentN;
        if(n->parent) parentN = n->parent->index;
        else parentN = -1;

        out.write((char*) &parentN, sizeof(parentN));
    }
}

void PLTree::load(std::string infile){
    std::ifstream in(infile);
    load(in);
    in.close();
}

void PLTree::load(std::istream& in){
    std::cerr << "Loading PLTree model ...\n";

    in.read((char*) &k, sizeof(k));
    in.read((char*) &t, sizeof(t));
    for(size_t i = 0; i < t; ++i) {
        TreeNode *n = new TreeNode();
        in.read((char*) &n->index, sizeof(n->index));
        in.read((char*) &n->label, sizeof(n->label));

        tree.push_back(n);
        if (n->label >= 0) treeLeaves[n->label] = n;
    }

    int rootN;
    in.read((char*) &rootN, sizeof(rootN));
    treeRoot = tree[rootN];

    for(size_t i = 0; i < t; ++i) {
        TreeNode *n = tree[i];

        int parentN;
        in.read((char*) &parentN, sizeof(parentN));
        if(parentN >= 0) {
            tree[parentN]->children.push_back(n);
            n->parent = tree[parentN];
        }
    }

    std::cerr << "  Nodes: " << tree.size() << ", leaves: " << treeLeaves.size() << "\n";
}

void PLTree::printTree(TreeNode *root){
    if(root == nullptr) root = treeRoot;

    std::unordered_set<TreeNode*> nSet;
    std::queue<TreeNode*> nQueue;
    nQueue.push(root);
    nSet.insert(root);
    int depth = 0;

    while(!nQueue.empty()){
        TreeNode* n = nQueue.front();
        nQueue.pop();

        if(nSet.count(n->parent)){
            nSet.clear();
            std::cerr << "\nDepth " << ++depth << ":";
        }

        nSet.insert(n);
        std::cerr << " " << n->index;
        if(n->parent) std::cerr << "(" << n->parent->index << ")";
        for(auto c : n->children) nQueue.push(c);
    }

    std::cerr << "\n";
}
