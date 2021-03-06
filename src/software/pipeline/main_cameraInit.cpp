// This file is part of the AliceVision project.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <aliceVision/sfm/sfm.hpp>
#include <aliceVision/sfm/viewIO.hpp>
#include <aliceVision/sfm/sfmDataIO_json.hpp>
#include <aliceVision/sensorDB/parseDatabase.hpp>
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/system/cmdline.hpp>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

using namespace aliceVision;
using namespace aliceVision::sfm;

namespace po = boost::program_options;
namespace fs = boost::filesystem;

/**
 * @brief Check that Kmatrix is a string like "f;0;ppx;0;f;ppy;0;0;1"
 * @param[in] Kmatrix
 * @param[out] focal
 * @param[out] ppx
 * @param[out] ppy
 * @return true if the string is correct
 */
bool checkIntrinsicStringValidity(const std::string& Kmatrix,
                                  double& focal,
                                  double& ppx,
                                  double& ppy)
{
  std::vector<std::string> vec_str;
  boost::split(vec_str, Kmatrix, boost::is_any_of(";"));
  if (vec_str.size() != 9)
  {
    ALICEVISION_LOG_ERROR("In K matrix string, missing ';' character");
    return false;
  }

  // Check that all K matrix value are valid numbers
  for (size_t i = 0; i < vec_str.size(); ++i)
  {
    double readvalue = 0.0;
    std::stringstream ss;
    ss.str(vec_str[i]);
    if(!(ss >> readvalue))
    {
      ALICEVISION_LOG_ERROR("In K matrix string, used an invalid not a number character");
      return false;
    }
    if (i==0) focal = readvalue;
    if (i==2) ppx = readvalue;
    if (i==5) ppy = readvalue;
  }
  return true;
}

/**
 * @brief Recursively list all files from a folder with a specific extension
 * @param[in] folderOrFile A file or foder path
 * @param[in] extensions An extensions filter
 * @param[out] outFiles A list of output image paths
 * @return true if folderOrFile have been load successfully
 */
bool listFiles(const std::string& folderOrFile,
               const std::vector<std::string>& extensions,
               std::vector<std::string>& resources)
{
  if(fs::is_regular_file(folderOrFile))
  {
    std::string fileExtension = fs::extension(folderOrFile);
    std::transform(fileExtension.begin(), fileExtension.end(), fileExtension.begin(), ::tolower);
    for(const std::string& extension: extensions)
    {
      if(fileExtension == extension)
      {
        resources.push_back(folderOrFile);
        return true;
      }
    }
  }
  else if(fs::is_directory(folderOrFile))
  {
    // list all files of the folder
    std::vector<std::string> allFiles;

    fs::directory_iterator endItr;
    for(fs::directory_iterator itr(folderOrFile); itr != endItr; ++itr)
      allFiles.push_back(itr->path().string());

    bool hasFile = false;
    for(const std::string& filePath: allFiles)
    {
      if(listFiles(filePath, extensions, resources))
        hasFile = true;
    }
    return hasFile;
  }
  ALICEVISION_LOG_ERROR("'" << folderOrFile << "' is not a valid folder or file path.");
  return false;
}


/**
 * @brief Create the description of an input image dataset for AliceVision toolsuite
 * - Export a SfMData file with View & Intrinsic data
 */
int main(int argc, char **argv)
{
  // command-line parameters

  std::string verboseLevel = system::EVerboseLevel_enumToString(system::Logger::getDefaultVerboseLevel());
  std::string sfmFilePath;
  std::string imageFolder;
  std::string sensorDatabasePath;
  std::string outputFilePath;

  // user optional parameters

  std::string defaultIntrinsicKMatrix;
  std::string defaultCameraModelName;
  double defaultFocalLengthPixel = -1.0;
  double defaultFieldOfView = -1.0;
  int groupCameraModel = 2;
  bool allowIncompleteOutput = false;
  bool allowSingleView = false;

  po::options_description allParams("AliceVision cameraInit");

  po::options_description requiredParams("Required parameters");
  requiredParams.add_options()
    ("input,i", po::value<std::string>(&sfmFilePath)->default_value(sfmFilePath),
      "a SfMData file (*.sfm).")
    ("imageFolder", po::value<std::string>(&imageFolder)->default_value(imageFolder),
      "Input images folder.")
    ("sensorDatabase,s", po::value<std::string>(&sensorDatabasePath)->required(),
      "Camera sensor width database path.")
    ("output,o", po::value<std::string>(&outputFilePath)->default_value("cameraInit.sfm"),
      "Output file path for the new SfMData file");

  po::options_description optionalParams("Optional parameters");
  optionalParams.add_options()
    ("defaultFocalLengthPix", po::value<double>(&defaultFocalLengthPixel)->default_value(defaultFocalLengthPixel),
      "Focal length in pixels. (or '-1' to unset)")
    ("defaultFieldOfView", po::value<double>(&defaultFieldOfView)->default_value(defaultFieldOfView),
      "Empirical value for the field of view in degree. (or '-1' to unset)")
    ("defaultIntrinsic", po::value<std::string>(&defaultIntrinsicKMatrix)->default_value(defaultIntrinsicKMatrix),
      "Intrinsics Kmatrix \"f;0;ppx;0;f;ppy;0;0;1\".")
    ("defaultCameraModel", po::value<std::string>(&defaultCameraModelName)->default_value(defaultCameraModelName),
      "Camera model type (pinhole, radial1, radial3, brown, fisheye4, fisheye1).")
    ("groupCameraModel", po::value<int>(&groupCameraModel)->default_value(groupCameraModel),
      "* 0: each view have its own camera intrinsic parameters\n"
      "* 1: view share camera intrinsic parameters based on metadata, if no metadata each view has its own camera intrinsic parameters\n"
      "* 2: view share camera intrinsic parameters based on metadata, if no metadata they are grouped by folder\n")
    ("allowIncompleteOutput", po::value<bool>(&allowIncompleteOutput)->default_value(allowIncompleteOutput),
      "Allow the program to output an incomplete SfMData file.\n"
      "Warning: if incomplete the output file can't be use in another program and should be post-process.")
    ("allowSingleView", po::value<bool>(&allowSingleView)->default_value(allowSingleView),
      "Allow the program to process a single view.\n"
      "Warning: if a single view is process, the output file can't be use in many other programs.");

  po::options_description logParams("Log parameters");
  logParams.add_options()
    ("verboseLevel,v", po::value<std::string>(&verboseLevel)->default_value(verboseLevel),
      "verbosity level (fatal, error, warning, info, debug, trace).");

  allParams.add(requiredParams).add(optionalParams).add(logParams);

  po::variables_map vm;
  try
  {
    po::store(po::parse_command_line(argc, argv, allParams), vm);

    if(vm.count("help") || (argc == 1))
    {
      ALICEVISION_COUT(allParams);
      return EXIT_SUCCESS;
    }
    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }

  ALICEVISION_COUT("Program called with the following parameters:");
  ALICEVISION_COUT(vm);

  // set verbose level
  system::Logger::get()->setLogLevel(verboseLevel);

  // set user camera model
  camera::EINTRINSIC defaultCameraModel = camera::EINTRINSIC::PINHOLE_CAMERA_START;
  if(!defaultCameraModelName.empty())
      defaultCameraModel = camera::EINTRINSIC_stringToEnum(defaultCameraModelName);

  // check user choose at least one input option
  if(imageFolder.empty() && sfmFilePath.empty())
  {
    ALICEVISION_LOG_ERROR("Program need -i or -f option");
    return EXIT_FAILURE;
  }

  // check user don't choose both input options
  if(!imageFolder.empty() && !sfmFilePath.empty())
  {
    ALICEVISION_LOG_ERROR("Cannot combine -i and -f options");
    return EXIT_FAILURE;
  }

  // check input folder
  if(!imageFolder.empty() && !fs::exists(imageFolder) && !fs::is_directory(imageFolder))
  {
    ALICEVISION_LOG_ERROR("The input folder doesn't exist");
    return EXIT_FAILURE;
  }

  // check sfm file
  if(!sfmFilePath.empty() && !fs::exists(sfmFilePath) && !fs::is_regular_file(sfmFilePath))
  {
    ALICEVISION_LOG_ERROR("The input sfm file doesn't exist");
    return EXIT_FAILURE;
  }

  // check output  string
  if(outputFilePath.empty())
  {
    ALICEVISION_LOG_ERROR("Invalid output");
    return EXIT_FAILURE;
  }

  // check if output folder exists, if no create it
  {
    const std::string outputFolderPart = fs::path(outputFilePath).parent_path().string();

    if(!outputFolderPart.empty() && !fs::exists(outputFolderPart))
    {
      if(!fs::create_directory(outputFolderPart))
      {
        ALICEVISION_LOG_ERROR("Cannot create output folder");
        return EXIT_FAILURE;
      }
    }
  }

  // check user don't combine intrinsic options
  if(!defaultIntrinsicKMatrix.empty() && defaultFocalLengthPixel > 0)
  {
    ALICEVISION_LOG_ERROR("Cannot combine --defaultIntrinsic --defaultFocalLengthPix options");
    return EXIT_FAILURE;
  }

  if(!defaultIntrinsicKMatrix.empty() && defaultFieldOfView > 0)
  {
    ALICEVISION_LOG_ERROR("Cannot combine --defaultIntrinsic --defaultFieldOfView options");
    return EXIT_FAILURE;
  }

  if(defaultFocalLengthPixel > 0 && defaultFieldOfView > 0)
  {
    ALICEVISION_LOG_ERROR("Cannot combine --defaultFocalLengthPix --defaultFieldOfView options");
    return EXIT_FAILURE;
  }

  // read K matrix if valid
  double defaultPPx = -1.0;
  double defaultPPy = -1.0;

  if(!defaultIntrinsicKMatrix.empty() && !checkIntrinsicStringValidity(defaultIntrinsicKMatrix, defaultFocalLengthPixel, defaultPPx, defaultPPy))
  {
    ALICEVISION_LOG_ERROR("--defaultIntrinsic Invalid K matrix input");
    return EXIT_FAILURE;
  }

  // check sensor database
  std::vector<sensorDB::Datasheet> sensorDatabase;
  if(!sensorDatabasePath.empty())
  {
    if(!sensorDB::parseDatabase(sensorDatabasePath, sensorDatabase))
    {
      ALICEVISION_LOG_ERROR("Invalid input database '" << sensorDatabasePath << "', please specify a valid file.");
      return EXIT_FAILURE;
    }
  }

  // use current time as seed for random generator for intrinsic Id without metadata
  std::srand(std::time(0));

  std::vector<std::string> noMetadataImagePaths;
  std::map<std::pair<std::string, std::string>, std::string> unknownSensors; // key (make,model) value (first imagePath)

  SfMData sfmData;

  // number of views with an initialized intrinsic
  std::size_t completeViewCount = 0;

  // load known informations
  if(imageFolder.empty())
  {
    // fill SfMData from the JSON file
    sfm::loadJSON(sfmData, sfmFilePath, ESfMData(VIEWS|INTRINSICS|EXTRINSICS), true);
  }
  else
  {
    // fill SfMData with the images in the input folder
    Views& views = sfmData.GetViews();
    std::vector<std::string> imagePaths;

    if(listFiles(imageFolder, {".jpg", ".jpeg", ".tif", ".tiff", ".exr"},  imagePaths))
    {
      std::vector<View> incompleteViews(imagePaths.size());

      #pragma omp parallel for
      for(int i = 0; i < incompleteViews.size(); ++i)
      {
        View& view = incompleteViews.at(i);
        view.setImagePath(imagePaths.at(i));
        sfm::updateIncompleteView(view);
      }

      for(const auto& view : incompleteViews)
        views.emplace(view.getViewId(), std::make_shared<View>(view));
    }
    else
      return EXIT_FAILURE;
  }

  if(sfmData.GetViews().empty())
  {
    ALICEVISION_LOG_ERROR("Can't find views in input.");
    return EXIT_FAILURE;
  }

  // create missing intrinsics
  auto viewPairItBegin = sfmData.GetViews().begin();

  #pragma omp parallel for
  for(int i = 0; i < sfmData.GetViews().size(); ++i)
  {
    View& view = *(std::next(viewPairItBegin,i)->second);
    IndexT intrinsicId = view.getIntrinsicId();
    double sensorWidth = -1;
    const bool hasCameraMetadata = (view.hasMetadata("Make") && view.hasMetadata("Model"));

    // check if the view intrinsic is already defined
    if(intrinsicId != UndefinedIndexT)
    {
      std::shared_ptr<camera::IntrinsicBase> intrinsic = sfmData.GetIntrinsicSharedPtr(view.getIntrinsicId());
      if(intrinsic != nullptr)
      {
        if(intrinsic->initialFocalLengthPix() > 0)
        {
          // the view intrinsic is initialized
          #pragma omp atomic
          ++completeViewCount;
        }
        else
        {
          // intrinsic px focal length is undefined
          // check if it is because the sensor is not in the database
          aliceVision::sensorDB::Datasheet datasheet;
          if(hasCameraMetadata && !getInfo(view.getMetadata("Make"), view.getMetadata("Model"), sensorDatabase, datasheet))
          {
            #pragma omp critical
            unknownSensors.emplace(std::make_pair(view.getMetadata("Make"),view.getMetadata("Model")), view.getImagePath()); // will throw an error message
          }
        }
        // don't need to build a new intrinsic
        continue;
      }
    }

    // get view intrinsic sensor width
    if(hasCameraMetadata)
    {
      aliceVision::sensorDB::Datasheet datasheet;
      if(getInfo(view.getMetadata("Make"), view.getMetadata("Model"), sensorDatabase, datasheet))
        sensorWidth = datasheet._sensorSize; // sensor is in the database
      else
      {
        #pragma omp critical
        unknownSensors.emplace(std::make_pair(view.getMetadata("Make"),view.getMetadata("Model")), view.getImagePath()); // will throw an error message
        if(!allowIncompleteOutput)
          continue;
      }
    }
    else
    {
      // no metadata 'Make' and 'Model' can't find sensor width
      #pragma omp critical
      noMetadataImagePaths.emplace_back(view.getImagePath()); // will throw a warning message

      if(allowIncompleteOutput)
      {
        view.setIntrinsicId(UndefinedIndexT);
        // don't build an intrinsic
        continue;
      }
    }

    // build intrinsic
    std::shared_ptr<camera::IntrinsicBase> intrinsic = getViewIntrinsic(view, sensorWidth, defaultFocalLengthPixel, defaultFieldOfView, defaultCameraModel, defaultPPx, defaultPPy);

    if(intrinsic->initialFocalLengthPix() > 0)
    {
      // the view intrinsic is initialized
      #pragma omp atomic
      ++completeViewCount;
    }

    // override serial number if necessary
    if(!hasCameraMetadata)
    {
      if(groupCameraModel == 2)
      {
        // when we have no metadata at all, we create one intrinsic group per folder.
        // the use case is images extracted from a video without metadata and assumes fixed intrinsics in the video.
        intrinsic->setSerialNumber(fs::path(view.getImagePath()).parent_path().string());
      }

      if(view.isPartOfRig())
      {
        // when we have no metadata for rig images, we create an intrinsic per camera.
        intrinsic->setSerialNumber("no_metadata_rig_" + std::to_string(view.getRigId()) + "_" + std::to_string(view.getSubPoseId()));
      }
    }

    // create intrinsic id
    // group camera that share common properties (leads to more faster & stable BA).
    if(intrinsicId == UndefinedIndexT)
      intrinsicId = intrinsic->hashValue();

    // don't group camera that share common properties
    if(groupCameraModel == 0)
      intrinsicId = std::rand(); // random number

    #pragma omp critical
    {
      view.setIntrinsicId(intrinsicId);
      sfmData.GetIntrinsics().emplace(intrinsicId, intrinsic);
    }
  }

  if(!noMetadataImagePaths.empty())
  {
    ALICEVISION_LOG_WARNING("No metadata in image(s) :");
    for(const auto& imagePath : noMetadataImagePaths)
      ALICEVISION_LOG_WARNING("\t- '" << imagePath << "'");
  }

  if(!unknownSensors.empty())
  {
    ALICEVISION_LOG_ERROR("Sensor width doesn't exist in the database for image(s) :");
    for(const auto& unknownSensor : unknownSensors)
      ALICEVISION_LOG_ERROR("image: '" << fs::path(unknownSensor.second).filename().string() << "'" << std::endl
                        << "\t- camera brand: " << unknownSensor.first.first <<  std::endl
                        << "\t- camera model: " << unknownSensor.first.second <<  std::endl);
    ALICEVISION_LOG_ERROR("Please add camera model(s) and sensor width(s) in the database." << std::endl);
    if(!allowIncompleteOutput)
      return EXIT_FAILURE;
  }

  if(!allowIncompleteOutput && (completeViewCount < 1 || (completeViewCount < 2 && !allowSingleView)))
  {
    ALICEVISION_LOG_ERROR("At least " << std::string(allowSingleView ? "one image" : "two images") << " should have an initialized intrinsic." << std::endl
                          << "Check your input images metadata (brand, model, focal length, ...), more should be set and correct." << std::endl);
    return EXIT_FAILURE;
  }

  // store SfMData views & intrinsic data
  if(!Save(sfmData, outputFilePath, ESfMData(VIEWS|INTRINSICS|EXTRINSICS)))
  {
    return EXIT_FAILURE;
  }

  // print report
  ALICEVISION_LOG_INFO("CameraInit report:" << std::endl
                   << "\t- # views listed in SfMData: " << sfmData.GetViews().size() << std::endl
                   << "\t- # views with an initialized intrinsic listed in SfMData: " << completeViewCount << std::endl
                   << "\t- # intrinsics listed in SfMData: " << sfmData.GetIntrinsics().size());

  return EXIT_SUCCESS;
}
