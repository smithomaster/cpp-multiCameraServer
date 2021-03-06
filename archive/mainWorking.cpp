/*----------------------------------------------------------------------------*/
/* Copyright (c) 2018 FIRST. All Rights Reserved.                             */
/* Open Source Software - may be modified and shared by FRC teams. The code   */
/* must be accompanied by the FIRST BSD license file in the root directory of */
/* the project.                                                               */
/*----------------------------------------------------------------------------*/

#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "GripCargoPipeline.h"
#include "GripStripPipeline.h"
#include "GripHatchPipeline.h"
#include <networktables/NetworkTableInstance.h>
#include <vision/VisionPipeline.h>
#include <vision/VisionRunner.h>
#include <wpi/StringRef.h>
#include <wpi/json.h>
#include <wpi/raw_istream.h>
#include <wpi/raw_ostream.h>
#include <opencv2/core/core.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include "cameraserver/CameraServer.h"
#include <networktables/NetworkTableEntry.h>
#include <networktables/NetworkTable.h>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/video/video.hpp>

/*
   JSON format:
   {
       "team": <team number>,
       "ntmode": <"client" or "server", "client" if unspecified>
       "cameras": [
           {
               "name": <camera name>
               "path": <path, e.g. "/dev/video0">
               "pixel format": <"MJPEG", "YUYV", etc>   // optional
               "width": <video mode width>              // optional
               "height": <video mode height>            // optional
               "fps": <video mode fps>                  // optional
               "brightness": <percentage brightness>    // optional
               "white balance": <"auto", "hold", value> // optional
               "exposure": <"auto", "hold", value>      // optional
               "properties": [                          // optional
                   {
                       "name": <property name>
                       "value": <property value>
                   }
               ],
               "stream": {                              // optional
                   "properties": [
                       {
                           "name": <stream property name>
                           "value": <stream property value>
                       }
                   ]
               }
           }
       ]
   }
 */

 const int k_HResolution = 320;
 const int k_VResolution = 240;
 const int k_WCameraHFOV = 128;
 const int k_WCameraVFOV = 96;
 const int k_LightCamCameraHFOV = 52;
 const int k_LightCamCameraVFOV = 39;

static const char* configFile = "/boot/frc.json";

namespace {

unsigned int team;
bool server = false;

struct CameraConfig {
  std::string name;
  std::string path;
  wpi::json config;
  wpi::json streamConfig;
};

std::vector<CameraConfig> cameraConfigs;

wpi::raw_ostream& ParseError() {
  return wpi::errs() << "config error in '" << configFile << "': ";
}

bool ReadCameraConfig(const wpi::json& config) {
  CameraConfig c;

  // name
  try {
    c.name = config.at("name").get<std::string>();
  } catch (const wpi::json::exception& e) {
    ParseError() << "could not read camera name: " << e.what() << '\n';
    return false;
  }

  // path
  try {
    c.path = config.at("path").get<std::string>();
  } catch (const wpi::json::exception& e) {
    ParseError() << "camera '" << c.name
                 << "': could not read path: " << e.what() << '\n';
    return false;
  }

  // stream properties
  if (config.count("stream") != 0) c.streamConfig = config.at("stream");

  c.config = config;

  cameraConfigs.emplace_back(std::move(c));
  return true;
}

bool ReadConfig() {
  // open config file
  std::error_code ec;
  wpi::raw_fd_istream is(configFile, ec);
  if (ec) {
    wpi::errs() << "could not open '" << configFile << "': " << ec.message()
                << '\n';
    return false;
  }

  // parse file
  wpi::json j;
  try {
    j = wpi::json::parse(is);
  } catch (const wpi::json::parse_error& e) {
    ParseError() << "byte " << e.byte << ": " << e.what() << '\n';
    return false;
  }

  // top level must be an object
  if (!j.is_object()) {
    ParseError() << "must be JSON object\n";
    return false;
  }

  // team number
  try {
    team = j.at("team").get<unsigned int>();
  } catch (const wpi::json::exception& e) {
    ParseError() << "could not read team number: " << e.what() << '\n';
    return false;
  }

  // ntmode (optional)
  if (j.count("ntmode") != 0) {
    try {
      auto str = j.at("ntmode").get<std::string>();
      wpi::StringRef s(str);
      if (s.equals_lower("client")) {
        server = false;
      } else if (s.equals_lower("server")) {
        server = true;
      } else {
        ParseError() << "could not understand ntmode value '" << str << "'\n";
      }
    } catch (const wpi::json::exception& e) {
      ParseError() << "could not read ntmode: " << e.what() << '\n';
    }
  }

  // cameras
  try {
    for (auto&& camera : j.at("cameras")) {
      if (!ReadCameraConfig(camera)) return false;
    }
  } catch (const wpi::json::exception& e) {
    ParseError() << "could not read cameras: " << e.what() << '\n';
    return false;
  }

  return true;
}

cs::UsbCamera StartCamera(const CameraConfig& config) {
  wpi::outs() << "Starting camera '" << config.name << "' on " << config.path
              << '\n';
  auto inst = frc::CameraServer::GetInstance();
  cs::UsbCamera camera{config.name, config.path};
  auto server = inst->StartAutomaticCapture(camera);

  camera.SetConfigJson(config.config);
  // camera.SetConnectionStrategy(cs::VideoSource::kConnectionKeepOpen);

  if (config.streamConfig.is_object())
    server.SetConfigJson(config.streamConfig);
    return camera;
}

// example pipeline
/*
class MyPipeline : public frc::VisionPipeline {
 public:
  int val = 0;

  void Process(cv::Mat& mat) override {
    ++val;
  }
};
*/
}  // namespace

int main(int argc, char* argv[]) {
  if (argc >= 2) configFile = argv[1];

  // read configuration
  if (!ReadConfig()) return EXIT_FAILURE;

  // start NetworkTables
  auto ntinst = nt::NetworkTableInstance::GetDefault();
  if (server) {
    wpi::outs() << "Setting up NetworkTables server\n";
    ntinst.StartServer();
  } else {
    wpi::outs() << "Setting up NetworkTables client for team " << team << '\n';
    ntinst.StartClientTeam(team);
  }

  // start cameras
  std::vector<cs::VideoSource> cameras;
  for (auto&& cameraConfig : cameraConfigs)
    cameras.emplace_back(StartCamera(cameraConfig));

  // start image processing on camera 0 if present
  if (cameras.size() >= 1) {
    std::thread([&] {
      // frc::VisionRunner<MyPipeline> runner(cameras[0], new MyPipeline(),
      //                                      [&](MyPipeline &pipeline) {
      //
      // });
      /* something like this for GRIP:
      frc::VisionRunner<MyPipeline> runner(cameras[0], new grip::GripPipeline(),
                                           [&](grip::GripPipeline& pipeline) {
        ...
      });
       */

      //runner.RunForever();

    const int kWidth = 320;
    const int kHeight = 240;

    // int object_X_Max=0;
    // int object_Y_Max=0;
    // int object_Y_Min=0;
    // int object_X_Min=0;
    // int centreX; //centre x
    // int centreY; //centre y
    // int objectWidth = 0;
    // int objectArea = 0;
    int xCrosshairOffset = 0;
    int yCrosshairOffset = 0;
    // double objectOffset = 0.0; // this value will output 0 at the leftmost pixel to 1 at the right-most pixel
    // double objectAngle = 0.0; //will give an angle from 0 to half of the fov, will be positive on the right hand side, left side is negative
    // double distanceFromObject = 0.0;
    // double widthCargo = 0.28;
    // double widthHatch = 0.44;
    // double widthStrip = 0.325;
    // std::string puttedText = "NULL";

  /*  //Cargo pipeline
    int cargo_Object_X_Max=0;
    int cargo_Object_Y_Max=0;
    int cargo_Object_Y_Min=0;
    int cargo_Object_X_Min=0;
    int cargo_CentreX; //centre x
    int cargo_CentreY; //centre y
    int cargo_ObjectWidth = 0;
    int cargo_ObjectArea = 0;
    //Hatch pipleine
    int hatch_Object_X_Max=0;
    int hatch_Object_Y_Max=0;
    int hatch_Object_Y_Min=0;
    int hatch_Object_X_Min=0;
    int hatch_CentreX; //centre x
    int hatch_CentreY; //centre y
    int hatch_ObjectWidth = 0;
    int hatch_ObjectArea = 0;
    //Strip pipeline
    int strip_Object_X_Max=0;
    int strip_Object_Y_Max=0;
    int strip_Object_Y_Min=0;
    int strip_Object_X_Min=0;
    int strip_CentreX; //centre x
    int strip_CentreY; //centre y
    int strip_ObjectWidth = 0;
    int strip_ObjectArea = 0;
    */

    // nt::NetworkTableEntry distanceCargoEntry;
    // nt::NetworkTableEntry offsetCargoEntry;
    // nt::NetworkTableEntry objectAngleCargoEntry;
    // nt::NetworkTableEntry distanceHatchEntry;
    // nt::NetworkTableEntry offsetHatchEntry;
    // nt::NetworkTableEntry objectAngleHatchEntry;
    // nt::NetworkTableEntry distanceStripEntry;
    // nt::NetworkTableEntry offsetStripEntry;
    // nt::NetworkTableEntry objectAngleStripEntry;
    //
    // auto inst = nt::NetworkTableInstance::GetDefault();
    // auto cargoTable = inst.GetTable("CargoOutputValues");
    // auto hatchTable = inst.GetTable("HatchOutputValues");
    // auto stripTable = inst.GetTable("StripOutputValues");
    //
    // distanceCargoEntry = cargoTable -> GetEntry("distanceCargo");
    // offsetCargoEntry = cargoTable -> GetEntry("offsetCargo");
    // objectAngleCargoEntry = cargoTable -> GetEntry("objectAngleCargo");
    // distanceHatchEntry = hatchTable -> GetEntry("distanceHatch");
    // offsetHatchEntry = hatchTable -> GetEntry("offsetHatch");
    // objectAngleHatchEntry = hatchTable -> GetEntry("objectAngleHatch");
    // distanceStripEntry = stripTable -> GetEntry("distanceStrip");
    // offsetStripEntry = stripTable -> GetEntry("offsetStrip");
    // objectAngleStripEntry = stripTable -> GetEntry("objectAngleStrip");



    cameras[0].SetResolution(kWidth, kHeight);
    cameras[1].SetResolution(kWidth, kHeight);
    cameras[2].SetResolution(kWidth, kHeight);
    cs::CvSink frontSink = frc::CameraServer::GetInstance()->GetVideo(cameras[0]);
    cs::CvSink lifeCamSink = frc::CameraServer::GetInstance()->GetVideo(cameras[1]);
    cs::CvSink backSink = frc::CameraServer::GetInstance()->GetVideo(cameras[1]);
    // Setup a CvSource. This will send images back to the Dashboard
    cs::CvSource croppedFrontOutput =
        frc::CameraServer::GetInstance()->PutVideo("Front Cropped", kWidth, 80);
    cs::CvSource crosshairsOutput =
        frc::CameraServer::GetInstance()->PutVideo("Crosshairs", kWidth, kHeight);
    // cs::CvSource backOutput =
    //     frc::CameraServer::GetInstance()->PutVideo("Back Camera", kWidth, kHeight);
    // cs::CvSource pipelineOutputCargo =
    //     frc::CameraServer::GetInstance()->PutVideo("cargoPipeline", kWidth, kHeight);
    // cs::CvSource pipelineOutputHatch =
    //     frc::CameraServer::GetInstance()->PutVideo("hatchPipeline", kWidth, kHeight);
    // cs::CvSource pipelineOutputStrip =
    //     frc::CameraServer::GetInstance()->PutVideo("stripPipeline", kWidth, kHeight);

    // Mats are very memory expensive. Lets reuse this Mat.
    cv::Mat croppedFrontMat;
    cv::Mat crosshairsMat;
    // cv::Mat pipelineMat;
    //declaring grip pipelines
    // cargoGrip::GripCargoPipeline* cargoPipeline = new cargoGrip::GripCargoPipeline();
    // hatchGrip::GripHatchPipeline* hatchPipeline = new hatchGrip::GripHatchPipeline();
    // stripGrip::GripStripPipeline* stripPipeline = new stripGrip::GripStripPipeline();

    while (true) {
      const int thresh = 10;
      // Tell the CvSink to grab a frame from the camera and put it
      // in the source mat.  If there is an error notify the output.
      // if (frontSink.GrabFrame(wideFovMat) == 0) {
      //   // Send the output the error.
      //   croppedOutput.NotifyError(croppedSink.GetError());
      //   // skip the rest of the current iteration
      //   continue;
      // }
      if (lifeCamSink.GrabFrame(crosshairsMat) == 0) {
        // Send the output the error.
        crosshairsOutput.NotifyError(lifeCamSink.GetError());
        // skip the rest of the current iteration
        continue;
      }
      if (frontSink.GrabFrame(croppedFrontMat) == 0) {
        // Send the output the error.
        croppedFrontOutput.NotifyError(frontSink.GetError());
        // skip the rest of the current iteration
        continue;
      }
      // if (crosshairsSink.GrabFrame(crosshairsMat) == 0) {
      //   // Send the output the error.
      //   croppedOutput.NotifyError(crosshairsSink.GetError());
      //   // skip the rest of the current iteration
      //   continue;
      // }
      // Put a rectangle on the image (x, y, width, height)
      cv::Rect rectangle = cv::Rect(0,80,kWidth,80);
      croppedFrontMat = croppedFrontMat(rectangle);

      xCrosshairOffset = 0;
      yCrosshairOffset = 0;
      // add the crosshairs
      cv::line(crosshairsMat, cv::Point(160 + xCrosshairOffset, 80 + yCrosshairOffset), cv::Point(160 + xCrosshairOffset,105 + yCrosshairOffset), CV_RGB(255,0,0));    // vertical
      cv::line(crosshairsMat, cv::Point(160 + xCrosshairOffset, 135 + yCrosshairOffset), cv::Point(160 + xCrosshairOffset,160 + yCrosshairOffset), CV_RGB(255,0,0));   // vertical
      cv::line(crosshairsMat, cv::Point(120 + xCrosshairOffset, 120 + yCrosshairOffset), cv::Point(145 + xCrosshairOffset,120 + yCrosshairOffset), CV_RGB(255,0,0));   // horizontal
      cv::line(crosshairsMat, cv::Point(175 + xCrosshairOffset, 120 + yCrosshairOffset), cv::Point(200 + xCrosshairOffset,120 + yCrosshairOffset), CV_RGB(255,0,0));   // horizontal
      // Give the output stream a new image to display
      crosshairsOutput.PutFrame(crosshairsMat);
      croppedFrontOutput.PutFrame(croppedFrontMat);
      //Vision process

      //1st GripCargoPipeline
      //2nd GripHatchPipeline
      //3rd GripStripPipeline

      //
  //     cargoPipeline->Process(wideFovMat);
  //     pipelineMat = *(cargoPipeline->GetRgbThresholdOutput());
  //     //Vision pixel process
  //     struct Pixbgr
  //     {
  //       unsigned char b:8;
  //     };
  //     object_X_Max=0;
  //     object_Y_Max=0;
  //     objectArea = 0;
  //     objectWidth = 0;
  //     centreX = 0;
  //     centreY = 0;
  //     distanceFromObject = 0;
  //     objectAngle = 0;
  //     object_Y_Min=pipelineMat.rows-1;
  //     object_X_Min=pipelineMat.cols-1;
  //     for(int i = 0; i < pipelineMat.rows; i++)
  //     {
  //       const struct Pixbgr* Mi = pipelineMat.ptr<struct Pixbgr>(i);
  //       for(int j = 0; j < pipelineMat.cols; j++)
  //       {
  //         if (Mi[j].b > thresh)
  //         {
  //           if (j < object_X_Min)
  //             object_X_Min = j;
  //           if (j > object_X_Max)
  //             object_X_Max = j;
  //           if (i < object_Y_Min)
  //             object_Y_Min = i;
  //           if (i > object_Y_Max)
  //             object_Y_Max = i;
  //         }
  //       }
  //     }
  //
  //     //Calculate area
  //     objectArea = (object_X_Max-object_X_Min) * (object_Y_Max-object_Y_Min);
  //     std::cout << objectArea << std::endl;
  //     std::cout << object_X_Max << std::endl;
  //     std::cout << object_X_Min << std::endl;
  //     objectWidth = sqrt(objectArea);
  //     if (objectWidth != 0){
  //     distanceFromObject = (widthCargo)/(objectWidth/320);
  //
  //     //it is the average of the centres of object
  //     centreX = object_X_Max - object_X_Min;
  //     centreY = object_Y_Max - object_Y_Min;
  //     std::cout << centreX << std::endl;
  //     std::cout << centreY << std::endl;
  //     objectOffset = (centreX/k_HResolution) - 0.5; // this value will output 0 at the leftmost pixel to 1 at the right-most pixel,
  //     objectAngle = objectAngle*(k_WCameraHFOV); //will give an angle from 0 to half of the fov, will be positive on the right hand side, left side is negative
  //     //show text of variables
  //     //cv::putText(pipelineMat, "Centre is: (" << std::to_string(centreX) << ":" << std::to_string(centreY) << ")" , cvPoint(50,100), FONT_HERSHEY_SIMPLEX, 1, (0,200,200), 4);
  //     distanceCargoEntry.SetDouble(distanceFromObject);
  //     offsetCargoEntry.SetDouble(objectOffset);
  //     objectAngleCargoEntry.SetDouble(objectOffset);
  //     }
  //
  //     // pipelineOutputCargo.PutFrame(pipelineMat);
  //
  //
  //
  //     //reset constants
  //     //hatch
  //     // hatchPipeline->Process(wideFovMat);
  //     // pipelineMat = *(hatchPipeline->GetHsvThresholdOutput());
  //     //Vision pixel process
  //   /*  struct Pixbgr
  //     {
  //       unsigned char b:8;
  //     }; */
  //
  //     object_X_Max=0;
  //     object_Y_Max=0;
  //     objectArea = 0;
  //     objectWidth = 0;
  //     centreX = 0;
  //     centreY = 0;
  //     distanceFromObject = 0;
  //     objectAngle = 0;
  //     object_Y_Min=pipelineMat.rows-1;
  //     object_X_Min=pipelineMat.cols-1;
  //     for(int i = 0; i < pipelineMat.rows; i++)
  //     {
  //       const struct Pixbgr* Mi = pipelineMat.ptr<struct Pixbgr>(i);
  //       for(int j = 0; j < pipelineMat.cols; j++)
  //       {
  //         if (Mi[j].b > thresh)
  //         {
  //           if (j < object_X_Min)
  //             object_X_Min = j;
  //           if (j > object_X_Max)
  //             object_X_Max = j;
  //           if (i < object_Y_Min)
  //             object_Y_Min = i;
  //           if (i > object_Y_Max)
  //             object_Y_Max = i;
  //         }
  //       }
  //     }
  //
  //
  //     //Calculate area
  //     std::cout << object_X_Max << std::endl;
  //     std::cout << object_X_Min << std::endl;
  //     objectWidth = object_X_Max-object_X_Min;
  //      if (objectWidth != 0){
  //     distanceFromObject = (widthHatch)/(objectWidth/320);
  //
  //
  //     //show text of variables
  //
  //     //cv::putText(pipelineMat, "Centre is: (" + std::to_string(centreX) + ":" + std::to_string(centreY) + ")" , cvPoint(50,100), FONT_HERSHEY_SIMPLEX, 1, (0,200,200), 4);
  //     //it is the average of the centres of object
  //     centreX = object_X_Max - object_X_Min;
  //     centreY = object_Y_Max - object_Y_Min;
  //     std::cout << centreX << std::endl;
  //     std::cout << centreY << std::endl;
  //     objectOffset = (centreX/k_HResolution) - 0.5; // this value will output 0 at the leftmost pixel to 1 at the right-most pixel,
  //     objectAngle = objectAngle*(k_WCameraHFOV); //will give an angle from 0 to half of the fov, will be positive on the right hand side, left side is negative
  //     distanceHatchEntry.SetDouble(distanceFromObject);
  //     offsetHatchEntry.SetDouble(objectOffset);
  //     objectAngleHatchEntry.SetDouble(objectOffset);
  //     // pipelineOutputHatch.PutFrame(pipelineMat);
  //
  // }
  //
  //     //start of Strip pipeline
  //     stripPipeline->Process(crosshairsMat);
  //     pipelineMat = *(stripPipeline->GetHsvThresholdOutput());
  //     // Vision pixel process
  //   /*  struct Pixbgr
  //     {
  //       unsigned char b:8;
  //     }; */
  //
  //     object_X_Max=0;
  //     object_Y_Max=0;
  //     objectArea = 0;
  //     objectWidth = 0;
  //     centreX = 0;
  //     centreY = 0;
  //     distanceFromObject = 0;
  //     objectAngle = 0;
  //
  //     object_Y_Min=pipelineMat.rows-1;
  //     object_X_Min=pipelineMat.cols-1;
  //     for(int i = 0; i < pipelineMat.rows; i++)
  //     {
  //       const struct Pixbgr* Mi = pipelineMat.ptr<struct Pixbgr>(i);
  //       for(int j = 0; j < pipelineMat.cols; j++)
  //       {
  //         if (Mi[j].b > thresh)
  //         {
  //           if (j < object_X_Min)
  //             object_X_Min = j;
  //           if (j > object_X_Max)
  //             object_X_Max = j;
  //           if (i < object_Y_Min)
  //             object_Y_Min = i;
  //           if (i > object_Y_Max)
  //             object_Y_Max = i;
  //         }
  //       }
  //     }
  //
  //
  //     //show text of variables
  //     //cv::putText(pipelineMat, "Centre is: (" + std::to_string(centreX) + ":" + std::to_string(centreY) + ")" , cvPoint(50,100), FONT_HERSHEY_SIMPLEX, 1, (0,200,200), 4);
  //     std::cout << object_X_Max << std::endl;
  //     std::cout << object_X_Min << std::endl;
  //     objectWidth = object_X_Max-object_X_Min;
  //
  //     if (objectWidth != 0){
  //
  //     distanceFromObject = (widthStrip)/(objectWidth/320);
  //     std::cout<< "Distance from strip "<< distanceFromObject << std::endl;
  //     //it is the average of the centres of object
  //     centreX = object_X_Max - object_X_Min;
  //     centreY = object_Y_Max - object_Y_Min;
  //     std::cout << centreX << std::endl;
  //     std::cout << centreY << std::endl;
  //     objectOffset = (centreX/k_HResolution) - 0.5; // this value will output 0 at the leftmost pixel to 1 at the right-most pixel,
  //     objectAngle = objectAngle*(k_WCameraHFOV); //will give an angle from 0 to half of the fov, will be positive on the right hand side, left side is negative
  //     distanceStripEntry.SetDouble(distanceFromObject);
  //     offsetStripEntry.SetDouble(objectOffset);
  //     objectAngleStripEntry.SetDouble(objectOffset);
  //     pipelineOutputStrip.PutFrame(pipelineMat);
  // }
      //reset constants

     }
   }).detach();
  }

  // loop forever
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(10));
}
