/*
 * Copyright 2014, 2015 Verkhovin Vyacheslav
 *
 * This file is part of RxCalc.
 *
 * RxCalc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RxCalc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RxCalc. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef STAGE_H
#define STAGE_H

#include <QString>
#include <math.h>

class Stage
{
public:
    enum priority
    {
        undifinited,
        input,
        output
    };
    Stage();
    ~Stage();
    // Stage params:
    void setName(QString name);
    QString name();
    void setEnabled(bool enabled);
    bool enabled();
    void setPowerGain(float powerGain);
    float powerGain();
    void setNoiseFigure(float noiseFigure);
    float noiseFigure();
    void setOip3(float oip3);
    float oip3();
    void setOp1db(float op1db);
    float op1db();
    void setIip3(float iip3);
    float iip3();
    void setIp1db(float ip1db);
    float ip1db();
    priority iip3Priority();
    priority ip1dbPriority();
    // System params:
    typedef struct
    {
        float powerGain;
        float noiseFigure;
        float iip3;
        float oip3;
        float ip1db;
        float op1db;
        float inputPower;
        float outputPower;
        float noiseFigureToSystemNoiseFigure;
        float stageIip3ToSystemIip3;
        float oip3StageToOp3System;
        float powerOutBackoff;
        float peakPowerOutBackoff;
    } sysStruct;
    sysStruct sys;

private:
    // Stage params - each with a value before the constructor sets it:
    // setPowerGain() reads the priorities and the IP3s and P1dBs, and the
    // constructor's first call of it read them unset (a value no priority
    // is, by UBSan).
    QString m_name;
    bool m_enabled = true;
    float m_powerGain = 0;
    float m_noiseFigure = 0;
    float m_oip3 = 0;
    float m_op1db = 0;
    float m_iip3 = 0;
    float m_ip1db = 0;
    priority m_iip3Priority = undifinited;
    priority m_ip1dbPriority = undifinited;
};

#endif // STAGE_H
