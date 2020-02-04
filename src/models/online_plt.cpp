/**
 * Copyright (c) 2019 by Marek Wydmuch
 * All rights reserved.
 */

#include "online_plt.h"
#include <cfloat>


OnlinePLT::OnlinePLT() {
    onlineTree = true;
    type = oplt;
    name = "Online PLT";
}

OnlinePLT::~OnlinePLT() {
    for (auto b : tmpBases) delete b;
}

void OnlinePLT::init(int labelCount, Args& args) {
    tree = new Tree();

    if (args.treeType == onlineBalanced || args.treeType == onlineComplete || args.treeType == onlineRandom ||
    args.treeType == onlineKMeans || args.treeType == onlineBestScore)
        onlineTree = true;
    else
        tree->buildTreeStructure(labelCount, args);

    if (!onlineTree) {
        bases.resize(tree->t);
        for (auto& b : bases) {
            b = new Base();
            b->setupOnlineTraining(args);
        }
    }
}

void OnlinePLT::update(const int row, Label* labels, size_t labelsSize, Feature* features, size_t featuresSize,
                       Args& args) {
    UnorderedSet<TreeNode*> nPositive;
    UnorderedSet<TreeNode*> nNegative;

    if (onlineTree) { // Check if example contains a new label
        std::vector<int> newLabels;

        {
            std::shared_lock<std::shared_timed_mutex> lock(treeMtx);
            for (int i = 0; i < labelsSize; ++i)
                if (!tree->leaves.count(labels[i])) newLabels.push_back(labels[i]);
        }

        if(!newLabels.empty()){ // Expand tree in case of the new label
            std::unique_lock<std::shared_timed_mutex> lock(treeMtx);
            expandTree(newLabels, features, args);
        }
    }

    {
        std::shared_lock<std::shared_timed_mutex> lock(treeMtx);
        getNodesToUpdate(nPositive, nNegative, labels, labelsSize);
    }

    // Update positive base estimators
    for (const auto &n : nPositive) bases[n->index]->update(1.0, features, args);

    // Update negative
    for (const auto &n : nNegative) bases[n->index]->update(0.0, features, args);

    // Update temporary nodes
    if (onlineTree)
        for (const auto &n : nPositive) {
            if (tmpBases[n->index] != nullptr)
                tmpBases[n->index]->update(0.0, features, args);
        }

    // Update centroids
    if (args.treeType == onlineKMeans){
        std::lock_guard<std::mutex> lock(centroidsMtx);

        for (const auto& n : nPositive){
            if(n->label == -1 || n->index != 0){

                if(n->index >= centroids.size()){
                    centroids.resize(n->index + 1);
                    norms.resize(n->index + 1);
                }

                UnorderedMap<int, float> &map = centroids[n->index];
                Feature *f = features;
                while (f->index != -1) {
                    if (f->index == 1){
                        ++f;
                        continue;
                    }
                    int index = f->index;
                    if (args.kMeansHash) index = hash(f->index) % args.hash;
                    map[index] += f->value;
                    ++f;
                }

                norms[n->index] = 0;
                for(const auto& w : map)
                    norms[n->index] += w.second * w.second;
                norms[n->index] = std::sqrt(norms[n->index]);
            }
        }
    }
}

void OnlinePLT::save(Args& args, std::string output) {

    // Save base classifiers
    std::ofstream out(joinPath(output, "weights.bin"));
    int size = bases.size();
    out.write((char*)&size, sizeof(size));
    for (int i = 0; i < bases.size(); ++i) {
        bases[i]->finalizeOnlineTraining(args);
        bases[i]->save(out);
    }
    out.close();

    // Save tree
    tree->saveToFile(joinPath(output, "tree.bin"));

    // Save tree structure
    tree->saveTreeStructure(joinPath(output, "tree"));
}

TreeNode* OnlinePLT::createTreeNode(TreeNode* parent, int label, Base* base, Base* tmpBase){
    auto n = tree->createTreeNode(parent, label);
    bases.push_back(base);
    tmpBases.push_back(tmpBase);

    return n;
}

void OnlinePLT::expandTree(const std::vector<Label>& newLabels, Feature* features, Args& args){

    //std::cerr << "  New labels in size of " << newLabels.size() << " ...\n";

    std::default_random_engine rng(args.getSeed());
    std::uniform_int_distribution<uint32_t> dist(0, args.arity - 1);

    if (tree->nodes.empty()) // Empty tree
        tree->root = createTreeNode(nullptr, -1, new Base(args), nullptr);  // Root node doesn't need tmp classifier

    if (tree->root->children.size() < args.arity) {
        TreeNode* newGroup = createTreeNode(tree->root, -1, new Base(args), new Base(args)); // Group node needs tmp classifier
        for(const auto nl : newLabels)
            createTreeNode(newGroup, nl, new Base(args), nullptr);
        newGroup->subtreeLeaves += newLabels.size();
        tree->root->subtreeLeaves += newLabels.size();
        return;
    }

    TreeNode* toExpand = tree->root;

    //std::cerr << "  Looking for node to expand ...\n";

    int depth = 0;
    float alfa = args.onlineTreeAlfa;
    while (tmpBases[toExpand->index] == nullptr) { // Stop when we reach expandable node
        ++depth;

        if (args.treeType == onlineRandom)
            toExpand = toExpand->children[dist(rng)];

        else if (args.treeType == onlineBestScore) { // Best score
            //std::cerr << "    Current node: " << toExpand->index << "\n";
            double bestScore = -DBL_MAX;
            TreeNode *bestChild;

            for (auto &child : toExpand->children) {
                double prob = bases[child->index]->predictProbability(features);
                double score = (1.0 - alfa) * prob + alfa * std::log(
                    (static_cast<double>(toExpand->subtreeLeaves) / toExpand->children.size()) / child->subtreeLeaves);
                if (score > bestScore) {
                    bestScore = score;
                    bestChild = child;
                }
                //std::cerr << "      Child " << child->index << " score: " << score << ", prob: " << prob << "\n";
            }
            toExpand = bestChild;
        }

        else if (args.treeType == onlineKMeans) { // Online K-Means tree
            //std::cerr << "  toExpand: " << toExpand->index << "\n";

            double bestScore = -DBL_MAX;
            TreeNode *bestChild;
            for (auto &child : toExpand->children) {
                double score = 0.0;
                UnorderedMap<int, float> &map = centroids[child->index];

                Feature *f = features;
                while (f->index != -1) {
                    if (f->index == 1){
                        ++f;
                        continue;
                    }
                    int index = f->index;
                    if(args.kMeansHash) index = hash(f->index) % args.hash;
                    auto w = map.find(index);
                    if (w != map.end()) score += (w->second / norms[child->index]) * f->value;
                    ++f;
                }

                score = (1.0 - alfa) * 1.0 / (1.0 + std::exp(score)) + alfa * std::log(
                        (static_cast<double>(toExpand->subtreeLeaves) / toExpand->children.size()) / child->subtreeLeaves);

                if (score > bestScore) {
                    bestScore = score;
                    bestChild = child;
                }

                //std::cerr << "  child: " << child->index << " " << score << "\n";
            }
            //std::cerr << bestScore << " " << bestChild->index << "\n";
            toExpand = bestChild;
        }

        toExpand->parent->subtreeLeaves += newLabels.size();
    }

    // Add labels
    for(int li = 0; li < newLabels.size(); ++li){
        Label nl = newLabels[li];
        if (toExpand->children.size() < args.maxLeaves) { // If there is still place in OVR
            ++toExpand->subtreeLeaves;
            auto newLabelNode = createTreeNode(toExpand, nl, tmpBases[toExpand->index]->copy());
            //std::cerr << "    Added node " << newLabelNode->index << " with label " << nl << " as " << toExpand->index << " child\n";
        } else {
            // If not, expand node
            bool inserted = false;

            //std::cerr << "    Looking for other free siblings...\n";

            for (auto &sibling : toExpand->parent->children) {
                if (sibling->children.size() < args.maxLeaves && tmpBases[sibling->index] != nullptr) {
                    auto newLabelNode = createTreeNode(sibling, nl, tmpBases[sibling->index]->copy());
                    ++sibling->subtreeLeaves;
                    inserted = true;

                    //std::cerr << "    Added node " << newLabelNode->index << " with label " << nl << " as " << sibling->index << " child\n";

                    break;
                }
            }

            if(inserted) continue;

           //std::cerr << "    Expanding " << toExpand->index << " node to bottom...\n";

            // Create the new node for children and move leaves to the new node
            TreeNode* newParentOfChildren = createTreeNode(nullptr, -1, tmpBases[toExpand->index]->copyInverted(), tmpBases[toExpand->index]->copy());
            for (auto& child : toExpand->children) tree->setParent(child, newParentOfChildren);
            toExpand->children.clear();
            tree->setParent(newParentOfChildren, toExpand);
            newParentOfChildren->subtreeLeaves = toExpand->subtreeLeaves;

            // Create new branch with new node
            auto newBranch = createTreeNode(toExpand, -1, tmpBases[toExpand->index]->copy(), new Base(args));
            createTreeNode(newBranch, nl, tmpBases[toExpand->index]->copy(), nullptr);

            // Remove temporary classifier
            if (toExpand->children.size() >= args.arity) {
                delete tmpBases[toExpand->index];
                tmpBases[toExpand->index] = nullptr;
            }

            toExpand->subtreeLeaves += newLabels.size() - li;
            toExpand = newBranch;
            ++toExpand->subtreeLeaves;
        }
    }

//    tree->printTree();
//    int x;
//    std::cin >> x;
}

