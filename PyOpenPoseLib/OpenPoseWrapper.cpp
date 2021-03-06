//
// Created by padeler on 21/6/2017.
//

#include "OpenPoseWrapper.h"

#include <glog/logging.h>

#include <openpose/core/headers.hpp>
#include <openpose/gui/headers.hpp>

#include <openpose/pose/headers.hpp>
#include <openpose/face/headers.hpp>
#include <openpose/hand/headers.hpp>

#include <openpose/utilities/headers.hpp>

// Detector specific paramters are here:
//#include <openpose/hand/handParameters.hpp>
//#include <openpose/face/faceParameters.hpp>
//#include <openpose/pose/poseParameters.hpp>

struct OpenPoseWrapper::PrivateData
{
    PrivateData(const op::Point<int> &netInputSize, const op::Point<int> &netOutputSize,
                const op::Point<int> &netInputSizeFace, const op::Point<int> &netOutputSizeFace,
                const op::Point<int> &outputSize, const op::PoseModel &poseModel,
                const std::string &modelFolder, int numScales, float scaleGap, float blendAlpha,
                const std::vector<op::HeatMapType> &heatMapTypes, const op::ScaleMode &heatMapScale):
            cvMatToOpInput{netInputSize, numScales, scaleGap}, cvMatToOpOutput{outputSize},
            opOutputToCvMat{outputSize},

            poseExtractorCaffe{netInputSize, netOutputSize, outputSize, numScales, poseModel, modelFolder, 0, heatMapTypes, heatMapScale},
            poseRenderer{netOutputSize, outputSize, poseModel, nullptr, 0.05, blendAlpha},

            faceExtractor{netInputSizeFace, netOutputSizeFace, modelFolder, 0},
            faceRenderer{outputSize, 0.4},
            faceDetector(poseModel),

            handDetector(poseModel),
            handRenderer{outputSize, 0.2},
            handExtractor{netInputSizeFace, netOutputSizeFace, modelFolder, 0}

    {}

    op::CvMatToOpInput cvMatToOpInput;
    op::CvMatToOpOutput cvMatToOpOutput;
    op::PoseExtractorCaffe poseExtractorCaffe;

    op::PoseRenderer poseRenderer;

    op::FaceExtractor faceExtractor;
    op::FaceDetector faceDetector;
    op::FaceRenderer faceRenderer;

    op::HandExtractor handExtractor;
    op::HandDetector handDetector;
    op::HandRenderer handRenderer;

    op::OpOutputToCvMat opOutputToCvMat;
};

OpenPoseWrapper::OpenPoseWrapper(const cv::Size &netPoseSize, const cv::Size &netFaceSize, const cv::Size &outSize,
                                 const std::string &model, const std::string &modelFolder, const int logLevel,
                                 bool downloadHeatmaps, OpenPoseWrapper::ScaleMode scaleMode, bool withFace, bool withHands):withFace(withFace), withHands(withHands) {
    google::InitGoogleLogging("OpenPose Wrapper");

    // Step 1 - Set logging level
    // - 0 will output all the logging messages
    // - 255 will output nothing

    op::check(0 <= logLevel && logLevel <= 255, "Wrong logging_level value.", __LINE__, __FUNCTION__, __FILE__);
    op::ConfigureLog::setPriorityThreshold((op::Priority)logLevel);

    // Step 2 - Init params
    op::Point<int> outputSize(outSize.width,outSize.height);
    op::Point<int> netInputSize(netPoseSize.width,netPoseSize.height);
    op::Point<int> netOutputSize = netInputSize;
    op::Point<int> netInputSizeFace(netFaceSize.width,netFaceSize.height);
    op::Point<int> netOutputSizeFace = netInputSizeFace;

    op::PoseModel poseModel;

    op::log("", op::Priority::Low, __LINE__, __FUNCTION__, __FILE__);
    if (model == "COCO")
        poseModel = op::PoseModel::COCO_18;
    else if (model == "MPI")
        poseModel = op::PoseModel::MPI_15;
    else if (model == "MPI_4_layers")
        poseModel = op::PoseModel::MPI_15_4;
    else
    {
        op::error("String does not correspond to any model (COCO, MPI, MPI_4_layers)", __LINE__, __FUNCTION__, __FILE__);
        poseModel = op::PoseModel::COCO_18;
    }

    int numScales = 1;
    float scaleGap = 0.3; // not used if numScales==1
    float blendAlpha = 0.6;

    //if you need to download bodypart heatmaps, Background or PAFs. They must be enabled here.
    std::vector<op::HeatMapType> hmt = {};
    if(downloadHeatmaps)
    {
        hmt = {op::HeatMapType::Parts, op::HeatMapType::Background, op::HeatMapType::PAFs};
    }

    // Step 3 - Initialize all required classes
    membersPtr = std::shared_ptr<PrivateData>(new PrivateData(netInputSize, netOutputSize,
                                                              netInputSizeFace, netOutputSizeFace,
                                                              outputSize, poseModel, modelFolder,
                                                              numScales, scaleGap, blendAlpha,
                                                              hmt, (op::ScaleMode)scaleMode));

    // Step 4 - Initialize resources on desired thread (in this case single thread, i.e. we init resources here)
    membersPtr->poseExtractorCaffe.initializationOnThread();
    membersPtr->poseRenderer.initializationOnThread();
    if(withFace) {
        membersPtr->faceExtractor.initializationOnThread();
        membersPtr->faceRenderer.initializationOnThread();
    }
    if(withHands) {
        membersPtr->handExtractor.initializationOnThread();
        membersPtr->handRenderer.initializationOnThread();
    }
}

void OpenPoseWrapper::detectPose(const cv::Mat &rgb) {
    // Step 2 - Format input image to OpenPose input and output formats
    op::Array<float> netInputArray;
    std::vector<float> scaleRatios;
    std::tie(netInputArray, scaleRatios) = membersPtr->cvMatToOpInput.format(rgb);
    // Step 3 - Estimate poseKeypoints
    membersPtr->poseExtractorCaffe.forwardPass(netInputArray, {rgb.cols, rgb.rows}, scaleRatios);
}

void OpenPoseWrapper::detectFace(const cv::Mat &rgb) {
    if(!withFace)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Face network was not initialized."));
    }
    const auto poseKeypoints = membersPtr->poseExtractorCaffe.getPoseKeypoints();
    const auto faceRectsOP = membersPtr->faceDetector.detectFaces(poseKeypoints, 1.0f);

    this->faceRects = cv::Mat(faceRectsOP.size(), 4, CV_32SC1, cv::Scalar(0));
    cv::Mat fr = this->faceRects.reshape(4,faceRects.rows); // stupid cv::Mat iterator cannot iterate over rows.
    std::transform(faceRectsOP.begin(), faceRectsOP.end(), fr.begin<cv::Vec4i>(), [](const op::Rectangle<float> &r) -> cv::Vec4i { return cv::Vec4i(r.x, r.y, r.width, r.height);});

    membersPtr->faceExtractor.forwardPass(faceRectsOP, rgb, 1.0f);
}

void OpenPoseWrapper::detectFace(const cv::Mat &rgb, const cv::Mat &faceRects)
{
    if(!withFace)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Face network was not initialized."));
    }
    if(faceRects.cols!=4 or faceRects.type()!=CV_32SC1)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Invalid face rectangles format. Expected Nx4 mat with type CV_32SC1"));
    }

    this->faceRects = faceRects; // keep a copy
    std::vector<op::Rectangle<float> > faceRectsOP(faceRects.rows);
    cv::Mat fr = faceRects.reshape(4,faceRects.rows); // stupid cv::Mat iterator cannot iterate over rows.
    std::transform(fr.begin<cv::Vec4i>(), fr.end<cv::Vec4i>(), faceRectsOP.begin(),
                   [](const cv::Vec4i &r) -> op::Rectangle<float> { return op::Rectangle<float>(r[0], r[1], r[2], r[3]);});

    membersPtr->faceExtractor.forwardPass(faceRectsOP, rgb, 1.0f);
}

void OpenPoseWrapper::detectHands(const cv::Mat &rgb) {
    if(!withHands)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Hand network was not initialized."));
    }

    const auto poseKeypoints = membersPtr->poseExtractorCaffe.getPoseKeypoints();
    const auto handRectsOP = membersPtr->handDetector.detectHands(poseKeypoints, 1.0f);

    this->handRects= cv::Mat(handRectsOP.size(), 8, CV_32SC1, cv::Scalar(0));
    cv::Mat hr = this->handRects.reshape(8,handRects.rows); // stupid cv::Mat iterator cannot iterate over rows.
    std::transform(handRectsOP.begin(), handRectsOP.end(), hr.begin<cv::Vec8i>(),
                   [](const std::array<op::Rectangle<float>, 2> &r) -> cv::Vec8i
                   { return cv::Vec8i(r[0].x, r[0].y, r[0].width, r[0].height, r[1].x, r[1].y, r[1].width, r[1].height); });

    membersPtr->handExtractor.forwardPass(handRectsOP, rgb, 1.0f);
}

void OpenPoseWrapper::detectHands(const cv::Mat &rgb, const cv::Mat &handRects)
{
    if(!withHands)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Hand network was not initialized."));
    }
    if(handRects.cols!=8 or handRects.type()!=CV_32SC1)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Invalid hand rectangles format. Expected Nx8 mat with type CV_32SC1"));
    }

    this->handRects = handRects;
    std::vector<std::array<op::Rectangle<float>, 2> > handRectsOP(handRects.rows);
    cv::Mat hr = handRects.reshape(8,handRects.rows); // stupid cv::Mat iterator cannot iterate over rows.
    std::transform(hr.begin<cv::Vec8i>(), hr.end<cv::Vec8i>(), handRectsOP.begin(),
                   [](const cv::Vec8i &r) -> std::array<op::Rectangle<float>, 2>
                   { return std::array<op::Rectangle<float>, 2>{op::Rectangle<float>(r[0], r[1], r[2], r[3]), op::Rectangle<float>(r[4], r[5], r[6], r[7])};});

    membersPtr->handExtractor.forwardPass(handRectsOP, rgb, 1.0f);
}

cv::Mat OpenPoseWrapper::render(const cv::Mat &rgb)
{
    double scaleInputToOutput;
    op::Array<float> outputArray;
    std::tie(scaleInputToOutput, outputArray) = membersPtr->cvMatToOpOutput.format(rgb);

    const auto poseKeypoints = membersPtr->poseExtractorCaffe.getPoseKeypoints();
    membersPtr->poseRenderer.renderPose(outputArray, poseKeypoints);

    if(withFace){
        const auto faceKeypoints = membersPtr->faceExtractor.getFaceKeypoints();
        membersPtr->faceRenderer.renderFace(outputArray, faceKeypoints);
    }
    if(withHands) {
        const auto handKeypoints = membersPtr->handExtractor.getHandKeypoints();
        membersPtr->handRenderer.renderHand(outputArray, handKeypoints);
    }

    auto outputImage = membersPtr->opOutputToCvMat.formatToCvMat(outputArray);
    return outputImage;
}

OpenPoseWrapper::KeypointGroups OpenPoseWrapper::getKeypoints(KeypointType kpt) {

    op::Array<float> faces,persons;
    std::array<op::Array<float>, 2> hands;

    KeypointGroups res;
    switch(kpt){
        case FACE:
            faces = membersPtr->faceExtractor.getFaceKeypoints();
            res.push_back(faces.getConstCvMat()); // all faces in a cv::Mat at index 0
            break;
        case HAND:
            hands = membersPtr->handExtractor.getHandKeypoints();
            res.push_back(hands[0].getConstCvMat()); // left hands cv::Mat
            res.push_back(hands[1].getConstCvMat()); // right hands cv::Mat
            break;
        default: // POSE
            persons = membersPtr->poseExtractorCaffe.getPoseKeypoints();
            res.push_back(persons.getConstCvMat()); // all persons in a cv::Mat at index 0
            break;
    }

    return res;
}

cv::Mat OpenPoseWrapper::getHeatmaps() {
    op::Array<float> maps = membersPtr->poseExtractorCaffe.getHeatMaps();
    return maps.getConstCvMat().clone();
}

