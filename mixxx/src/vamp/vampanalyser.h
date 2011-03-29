/*
 * vampanalyser.h
 *  Created on: 14/mar/2011
 *      Author: Vittorio Colao
 *      Original ideas taken from Audacity VampEffect class from Chris Cannam.
 */

#ifndef VAMPANALYSER_H_
#define VAMPANALYSER_H_

#include <vamp-hostsdk/PluginLoader.h>
#include <QString>
#include <QList>
#include <QVector>
#include "sampleutil.h"


 struct VampPluginEvent
        {
        int isFromOutput;

        bool hasStartingFrame;

        double StartingFrame;

        bool hasEndingFrame;

        double EndingFrame;

        int VectorValuesSize;

        QVector<float> Values;

        QString Label;
        };

 typedef QList<VampPluginEvent> VampPluginEventList;


class VampAnalyser {

public:

    VampAnalyser(Vamp::HostExt::PluginLoader::PluginKey key);
    virtual ~VampAnalyser();


    bool Init(int samplerate, const int TotalSamples);

    //virtual bool SetPluginParameter(QString parameter, QString value);

    virtual bool Process(const CSAMPLE *pIn, const int iLen);

    virtual VampPluginEventList GetResults();

    bool End();



private:

    void AddFeatures(Vamp::Plugin::FeatureSet &features);

    Vamp::HostExt::PluginLoader::PluginKey mKey;

    int m_iSampleCount, m_iOUT, m_iRemainingSamples,
        m_iBlockSize, m_iStepSize, mRate, m_iOutputs;
    CSAMPLE ** m_pluginbuf;
    Vamp::Plugin *mPlugin;
    Vamp::Plugin::ParameterList mParameters;
    VampPluginEventList m_Results;

};

#endif /* VAMPANALYSER_H_ */
