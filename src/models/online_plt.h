/**
 * Copyright (c) 2019 by Marek Wydmuch
 * All rights reserved.
 */

#pragma once

#include "plt.h"
#include "online_model.h"


class OnlinePLT: public OnlineModel, public PLT {
public:
    OnlinePLT();
    ~OnlinePLT() override;

    void init(int labelCount, Args &args) override;
    void update(const int row, Label* labels, size_t labelsSize, Feature* features, size_t featuresSize, Args &args) override;
    void save(Args &args, std::string output) override;

protected:
    std::vector<Base*> tmpBases;
    std::mutex treeMtx;
};
