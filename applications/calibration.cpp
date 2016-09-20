/*-*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-   */
/*ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab:*/

/*
(C) Copyright 2016 Johns Hopkins University (JHU), All Rights Reserved.

--- begin cisst license - do not edit ---

This software is provided "as is" under an open source license, with
no warranty.  The complete license can be found in license.txt and
http://www.cisst.org/cisst/license.txt.

--- end cisst license ---
*/

#include <cisstConfig.h>
#include <cisstCommon/cmnLogger.h>
#include <cisstCommon/cmnKbHit.h>
#include <cisstCommon/cmnGetChar.h>
#include <cisstOSAbstraction/osaSleep.h>
#if CISST_HAS_JSON
#include <cisstVector/vctDataFunctionsFixedSizeVectorJSON.h>
#include <cisstVector/vctDataFunctionsFixedSizeMatrixJSON.h>
#endif
#include <cisstMultiTask/mtsTaskContinuous.h>
#include <cisstMultiTask/mtsInterfaceRequired.h>
#include <cisstNumerical/nmrLSqLin.h>
#include <sawOptoforceSensor/mtsOptoforce3D.h>

class SensorCalibration : public mtsTaskMain {

private:

    mtsFunctionRead GetForceCartesian;
    mtsFunctionRead IsCalibrated;
    mtsFunctionVoid Uncalibrate;
    mtsFunctionVoid Zero;
    mtsFunctionVoid UnZero;
    // Following are not used by this application
    mtsFunctionRead GetStatus;
    mtsFunctionRead GetForce;
    mtsFunctionRead GetConnected;
    mtsFunctionRead GetTaskPeriod;
    mtsFunctionRead GetSensorConfig;
    mtsFunctionWrite SetSensorConfig;
    mtsFunctionRead GetBias;
    mtsFunctionWrite SetBias;
    mtsFunctionRead GetLength;
    mtsFunctionWrite SetLength;
    mtsFunctionRead GetScale;
    mtsFunctionWrite SetScale;

protected:
    std::vector<vctDouble6> force_pos;    // The vector contains information of applied force and its location
    std::vector<vctDouble3> S_raw;        // The vector containing all raw sensor readings
    vctDouble3x6 Matrix_a;                // The calibration result

public:

    SensorCalibration() : mtsTaskMain("SensorCalibration")
    {
        mtsInterfaceRequired *req = AddInterfaceRequired("Input", MTS_OPTIONAL);
        if (req) {
            req->AddFunction("GetTaskPeriod", GetTaskPeriod);
            req->AddFunction("GetStatus", GetStatus);
            req->AddFunction("GetForce", GetForce);
            req->AddFunction("GetForceTorque", GetForceCartesian);
            req->AddFunction("GetConnected", GetConnected);
            req->AddFunction("GetSensorConfig", GetSensorConfig);
            req->AddFunction("SetSensorConfig", SetSensorConfig);
            req->AddFunction("IsCalibrated", IsCalibrated);
            req->AddFunction("Uncalibrate", Uncalibrate);
            req->AddFunction("Rebias", Zero);
            req->AddFunction("Unbias", UnZero);
            req->AddFunction("GetBias", GetBias);
            req->AddFunction("SetBias", SetBias);
            req->AddFunction("GetLength", GetLength);
            req->AddFunction("SetLength", SetLength);
            req->AddFunction("GetScale", GetScale);
            req->AddFunction("SetScale", SetScale);
        }
    }

    void Configure(const std::string&) {}

    void Startup() {
        bool flag;
        IsCalibrated(flag);
        if (flag) {
            std::cout << "Sensor is calibrated, removing calibration..." << std::endl;
            Uncalibrate();
        }
        std::cout << "Ready" << std::endl;
    }
    
    void Run()
    {
        std::cout << "1) Enter applied force (fx, fy, fz) and length (x, y, z)" << std::endl
                  << "2) Take corresponding sensor readings" << std::endl
                  << "3) Compute calibration matrix" << std::endl
                  << "4) Write results to JSON file" << std::endl
                  << "5) Exit" << std::endl
                  << "Select Option: " << std::endl;

        char c = cmnGetChar();
        switch (c) {
            case '1':   // Input length and applied force
                Zero();
                std::cout << "Please put the specified weight at the location you chose" << std::endl;
                GetForcePos();
                std::cout << "Input force = " << force_pos.back()[0] << " " << force_pos.back()[1] << " "
                          << force_pos.back()[2] << std::endl;
                std::cout << "Location of force = " << force_pos.back()[3] << " " << force_pos.back()[4] << " "
                          << force_pos.back()[5] << std::endl;
                break;
            case '2':   // Measurements with input weight
                GetSensorReading();
                std::cout << "Sensor readings = " << S_raw.back()[0] << " " << S_raw.back()[1] << " "
                          << S_raw.back()[2] << std::endl;
                std::cout << "Please remove the weight from the sensor" << std::endl;
                UnZero();
                break;
            case '3':   // Compute calibration matrix
                std::cout << "The number of groups of measurements you used to calculate calibration matrix = "
                          << force_pos.size() << std::endl;
                ComputeCalibration();
                break;
            case '4':   // Write results to a json file
                Serialization("output.json");
                break;
            case '5':   // quit program
                std::cout << "Exiting.. " << std::endl;
                this->Kill();
                break;
        }
    }

    // Prompt the user for the applied force and the location
    void GetForcePos() {

        vctDouble6 b;
        std::cout << "Please specify the force (N) used to calculate the calibration matrix" << std::endl;
        std::cin >> b[0] >> b[1] >> b[2];
        std::cout << "Please specify the position of the applied force" << std::endl;
        std::cin >> b[3] >> b[4] >> b[5];
        force_pos.push_back(b);
    }

    // Get an average sensor reading
    bool GetSensorReading()
    {
        prmForceCartesianGet force;
        vctDouble3 b;  //Temporal vector to save three sensor readings of each iteration
        b.SetAll(0);
        for (int i = 0; i < 50; i++) {
            GetForceCartesian(force);
            if (force.Valid()) {
                b.X() += force.Force().X();
                b.Y() += force.Force().Y();
                b.Z() += force.Force().Z();
            }
            else {
                std::cout << "Invalid force -- check connection and try again" << std::endl;
                return false;
            }
            osaSleep(0.1);
        }
        b.Divide(50);
        S_raw.push_back(b);
        return true;
    }
    
    // Solves F*a = S, where:
    //   - F is the matrix computed from the known force values and placements (distances)
    //     with respect to the sensor origin.
    //   - S is a vector of the raw sensor readings.
    //   - a is the solution, which contains the elements of the calibration matrix, arranged as a vector.
    void ComputeCalibration()
    {
        vctDynamicMatrix<double> Matrix_f(force_pos.size() * 3, 18, VCT_COL_MAJOR);
        Matrix_f.SetAll(0);
        vctDynamicVector<double> S(force_pos.size() * 3);
        S.SetAll(0);

        // Create the 3N x 18 matrix F and the 3N vector S
        for (size_t i = 0; i < force_pos.size(); i++) {
            Matrix_f[3 * i][0] = force_pos[i][0];
            Matrix_f[3 * i][1] = force_pos[i][1];
            Matrix_f[3 * i][2] = force_pos[i][2];
            Matrix_f[3 * i][3] = force_pos[i][5] * force_pos[i][1] - force_pos[i][4] * force_pos[i][2];
            Matrix_f[3 * i][4] = -force_pos[i][5] * force_pos[i][0] + force_pos[i][3] * force_pos[i][2];
            Matrix_f[3 * i][5] = force_pos[i][4] * force_pos[i][0] - force_pos[i][3] * force_pos[i][1];

            Matrix_f[3 * i + 1][6] = force_pos[i][0];
            Matrix_f[3 * i + 1][7] = force_pos[i][1];
            Matrix_f[3 * i + 1][8] = force_pos[i][2];
            Matrix_f[3 * i + 1][9] = force_pos[i][5] * force_pos[i][1] - force_pos[i][4] * force_pos[i][2];
            Matrix_f[3 * i + 1][10] = -force_pos[i][5] * force_pos[i][0] + force_pos[i][3] * force_pos[i][2];
            Matrix_f[3 * i + 1][11] = force_pos[i][4] * force_pos[i][0] - force_pos[i][3] * force_pos[i][1];

            Matrix_f[3 * i + 2][12] = force_pos[i][0];
            Matrix_f[3 * i + 2][13] = force_pos[i][1];
            Matrix_f[3 * i + 2][14] = force_pos[i][2];
            Matrix_f[3 * i + 2][15] = force_pos[i][5] * force_pos[i][1] - force_pos[i][4] * force_pos[i][2];
            Matrix_f[3 * i + 2][16] = -force_pos[i][5] * force_pos[i][0] + force_pos[i][3] * force_pos[i][2];
            Matrix_f[3 * i + 2][17] = force_pos[i][4] * force_pos[i][0] - force_pos[i][3] * force_pos[i][1];

            S[3 * i] = S_raw[i][0];
            S[3 * i + 1] = S_raw[i][1];
            S[3 * i + 2] = S_raw[i][2];
        }

        // nmrLSqLin alters the contents of Matrix_f and S, so it would be necessary to make
        // a copy if we wish to preserve their contents (in this case, it is not necessary).
        vctDynamicVector<double> Vector_a;
        nmrLSqLin(Matrix_f, S, Vector_a);

        // Now, copy the elements of Vector_a into the matrix.
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 6; k++) {
                Matrix_a[j][k] = Vector_a[j * 6 + k];
            }
        }
    }

    void Serialization(const std::string &filename)
    {
#if CISST_HAS_JSON
        std::ofstream jsonStream;
        Json::Value jsonConfig;
        Json::StyledWriter jsonWriter;

        Json::Value Matrix_cal, Force_Pos, SensorReading;
        cmnDataJSON<vctFixedSizeMatrix<double, 3, 6> >::SerializeText(Matrix_a, Matrix_cal);
        cmnDataJSON<std::vector<vctDouble6> >::SerializeText(force_pos, Force_Pos);
        cmnDataJSON<std::vector<vctDouble3> >::SerializeText(S_raw, SensorReading);
        jsonConfig["cal-matrix"] = Matrix_cal;
        jsonConfig["force-pos"] = Force_Pos;
        jsonConfig["raw-sensor-reading"] = SensorReading;

        jsonStream.open(filename.c_str());
        jsonStream << jsonWriter.write(jsonConfig) << std::endl;
        jsonStream.close();
#else
        std::cout << "Sorry, must compile cisst with JSON support" << std::endl;
#endif
    }

    void Cleanup() {}
};


int main(int argc, char **argv)
{
    cmnLogger::SetMask(CMN_LOG_ALLOW_ALL);
    cmnLogger::SetMaskDefaultLog(CMN_LOG_ALLOW_ALL);
    cmnLogger::SetMaskFunction(CMN_LOG_ALLOW_ALL);
    cmnLogger::SetMaskClass("mtsOptoforce3D", CMN_LOG_ALLOW_ALL);
    cmnLogger::AddChannel(std::cout, CMN_LOG_ALLOW_ERRORS_AND_WARNINGS);
    
    mtsOptoforce3D *opto;
    int portNum;
    if (sscanf(argv[1], "%d", &portNum) == 1) {
        std::cout << "Selecting port number: " << portNum << std::endl;
        opto = new mtsOptoforce3D("OptoforceSensor", portNum);
    }
    else {
        std::cout << "Selecting port name: " << argv[1] << std::endl;
        opto = new mtsOptoforce3D("OptoforceSensor", argv[1]);
    }

    mtsComponentManager * componentManager = mtsComponentManager::GetInstance();
    componentManager->AddComponent(opto);

    SensorCalibration client;
    componentManager->AddComponent(&client);

    if (!componentManager->Connect(client.GetName(), "Input", opto->GetName(), "Force")) {
        std::cout << "Failed to connect: "
            << client.GetName() << "::Input to "
            << opto->GetName() << "::Force" << std::endl;
        delete opto;
        return -1;
    }

    componentManager->CreateAll();

    componentManager->StartAll();

    // Main thread passed to client task
    opto->Kill();
    componentManager->WaitForStateAll(mtsComponentState::FINISHED, 2.0 * cmn_s);

    componentManager->Cleanup();

    // stop all logs
    cmnLogger::SetMask(CMN_LOG_ALLOW_NONE);
    delete opto;

    return 0;
}