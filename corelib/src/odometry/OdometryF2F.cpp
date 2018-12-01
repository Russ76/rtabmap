/*
Copyright (c) 2010-2016, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "rtabmap/core/odometry/OdometryF2F.h"
#include "rtabmap/core/OdometryInfo.h"
#include "rtabmap/core/Registration.h"
#include "rtabmap/core/EpipolarGeometry.h"
#include "rtabmap/core/util3d_transforms.h"
#include "rtabmap/utilite/ULogger.h"
#include "rtabmap/utilite/UTimer.h"
#include "rtabmap/utilite/UStl.h"

namespace rtabmap {

OdometryF2F::OdometryF2F(const ParametersMap & parameters) :
	Odometry(parameters),
	keyFrameThr_(Parameters::defaultOdomKeyFrameThr()),
	visKeyFrameThr_(Parameters::defaultOdomVisKeyFrameThr()),
	scanKeyFrameThr_(Parameters::defaultOdomScanKeyFrameThr())
{
	registrationPipeline_ = Registration::create(parameters);
	Parameters::parse(parameters, Parameters::kOdomKeyFrameThr(), keyFrameThr_);
	Parameters::parse(parameters, Parameters::kOdomVisKeyFrameThr(), visKeyFrameThr_);
	Parameters::parse(parameters, Parameters::kOdomScanKeyFrameThr(), scanKeyFrameThr_);
	UASSERT(keyFrameThr_>=0.0f && keyFrameThr_<=1.0f);
	UASSERT(visKeyFrameThr_>=0);
	UASSERT(scanKeyFrameThr_>=0.0f && scanKeyFrameThr_<=1.0f);

	parameters_ = parameters;
}

OdometryF2F::~OdometryF2F()
{
	delete registrationPipeline_;
}

void OdometryF2F::reset(const Transform & initialPose)
{
	Odometry::reset(initialPose);
	refFrame_ = Signature();
	lastKeyFramePose_.setNull();
}

// return not null transform if odometry is correctly computed
Transform OdometryF2F::computeTransform(
		SensorData & data,
		const Transform & guess,
		OdometryInfo * info)
{
	UTimer timer;
	Transform output;
	if(!data.rightRaw().empty() && !data.stereoCameraModel().isValidForProjection())
	{
		UERROR("Calibrated stereo camera required");
		return output;
	}
	if(!data.depthRaw().empty() &&
		(data.cameraModels().size() != 1 || !data.cameraModels()[0].isValidForProjection()))
	{
		UERROR("Calibrated camera required (multi-cameras not supported).");
		return output;
	}

	bool addKeyFrame = false;
	RegistrationInfo regInfo;

	UASSERT(!this->getPose().isNull());
	if(lastKeyFramePose_.isNull())
	{
		if(this->getPose().rotation().isIdentity() &&
			data.imu().linearAcceleration()[0]!=0.0 &&
			data.imu().linearAcceleration()[1]!=0.0 &&
			data.imu().linearAcceleration()[2]!=0.0 &&
			!data.imu().localTransform().isNull())
		{
			// align with gravity
			Eigen::Vector3f n(data.imu().linearAcceleration()[0], data.imu().linearAcceleration()[1], data.imu().linearAcceleration()[2]);
			n = data.imu().localTransform().toEigen3f() * n;
			n.normalize();
			n[0]*=-1;
			n[1]*=-1;
			n[2]*=-1;
			Eigen::Vector3f z(0,0,1);
			//get rotation from z to n;
			Eigen::Matrix3f R;
			R = Eigen::Quaternionf().setFromTwoVectors(n,z);
			Transform rotation(
					R(0,0), R(0,1), R(0,2), 0,
					R(1,0), R(1,1), R(1,2), 0,
					R(2,0), R(2,1), R(2,2), 0);
			this->reset(rotation);
		}

		lastKeyFramePose_ = this->getPose(); // reset to current pose
	}
	Transform motionSinceLastKeyFrame = lastKeyFramePose_.inverse()*this->getPose();

	Signature newFrame(data);
	if(refFrame_.sensorData().isValid())
	{
		float maxCorrespondenceDistance = 0.0f;
		float pmOutlierRatio = 0.0f;
		if(guess.isNull() &&
			!registrationPipeline_->isImageRequired() &&
			registrationPipeline_->isScanRequired() &&
			this->framesProcessed() < 2)
		{
			// only on initialization (first frame to register), increase icp max correspondences in case the robot is already moving
			maxCorrespondenceDistance = Parameters::defaultIcpMaxCorrespondenceDistance();
			pmOutlierRatio = Parameters::defaultIcpPMOutlierRatio();
			Parameters::parse(parameters_, Parameters::kIcpMaxCorrespondenceDistance(), maxCorrespondenceDistance);
			Parameters::parse(parameters_, Parameters::kIcpPMOutlierRatio(), pmOutlierRatio);
			ParametersMap params;
			params.insert(ParametersPair(Parameters::kIcpMaxCorrespondenceDistance(), uNumber2Str(maxCorrespondenceDistance*3.0f)));
			params.insert(ParametersPair(Parameters::kIcpPMOutlierRatio(), uNumber2Str(0.95f)));
			registrationPipeline_->parseParameters(params);
		}

		Signature tmpRefFrame = refFrame_;
		output = registrationPipeline_->computeTransformationMod(
				tmpRefFrame,
				newFrame,
				// special case for ICP-only odom, set guess to identity if we just started or reset
				!guess.isNull()?motionSinceLastKeyFrame*guess:!registrationPipeline_->isImageRequired()&&this->framesProcessed()<2?motionSinceLastKeyFrame:Transform(),
				&regInfo);

		if(maxCorrespondenceDistance>0.0f)
		{
			// set it back
			ParametersMap params;
			params.insert(ParametersPair(Parameters::kIcpMaxCorrespondenceDistance(), uNumber2Str(maxCorrespondenceDistance)));
			params.insert(ParametersPair(Parameters::kIcpPMOutlierRatio(), uNumber2Str(pmOutlierRatio)));
			registrationPipeline_->parseParameters(params);
		}

		if(output.isNull() && !guess.isNull() && registrationPipeline_->isImageRequired())
		{
			tmpRefFrame = refFrame_;
			// reset matches, but keep already extracted features in newFrame.sensorData()
			newFrame.setWords(std::multimap<int, cv::KeyPoint>());
			newFrame.setWords3(std::multimap<int, cv::Point3f>());
			newFrame.setWordsDescriptors(std::multimap<int, cv::Mat>());
			UWARN("Failed to find a transformation with the provided guess (%s), trying again without a guess.", guess.prettyPrint().c_str());
			// If optical flow is used, switch temporary to feature matching
			int visCorTypeBackup = Parameters::defaultVisCorType();
			Parameters::parse(parameters_, Parameters::kVisCorType(), visCorTypeBackup);
			if(visCorTypeBackup == 1)
			{
				ParametersMap params;
				params.insert(ParametersPair(Parameters::kVisCorType(), "0"));
				registrationPipeline_->parseParameters(params);
			}

			output = registrationPipeline_->computeTransformationMod(
				tmpRefFrame,
				newFrame,
				Transform(), // null guess
				&regInfo);

			if(visCorTypeBackup == 1)
			{
				ParametersMap params;
				params.insert(ParametersPair(Parameters::kVisCorType(), "1"));
				registrationPipeline_->parseParameters(params);
			}

			if(output.isNull())
			{
				UWARN("Trial with no guess still fail.");
			}
			else
			{
				UWARN("Trial with no guess succeeded.");
			}
		}

		if(info && this->isInfoDataFilled())
		{
			std::list<std::pair<int, std::pair<cv::KeyPoint, cv::KeyPoint> > > pairs;
			EpipolarGeometry::findPairsUnique(tmpRefFrame.getWords(), newFrame.getWords(), pairs);
			info->refCorners.resize(pairs.size());
			info->newCorners.resize(pairs.size());
			std::map<int, int> idToIndex;
			int i=0;
			for(std::list<std::pair<int, std::pair<cv::KeyPoint, cv::KeyPoint> > >::iterator iter=pairs.begin();
				iter!=pairs.end();
				++iter)
			{
				info->refCorners[i] = iter->second.first.pt;
				info->newCorners[i] = iter->second.second.pt;
				idToIndex.insert(std::make_pair(iter->first, i));
				++i;
			}
			info->cornerInliers.resize(regInfo.inliersIDs.size(), 1);
			i=0;
			for(; i<(int)regInfo.inliersIDs.size(); ++i)
			{
				info->cornerInliers[i] = idToIndex.at(regInfo.inliersIDs[i]);
			}

			Transform t = this->getPose()*motionSinceLastKeyFrame.inverse();
			for(std::multimap<int, cv::Point3f>::const_iterator iter=tmpRefFrame.getWords3().begin(); iter!=tmpRefFrame.getWords3().end(); ++iter)
			{
				info->localMap.insert(std::make_pair(iter->first, util3d::transformPoint(iter->second, t)));
			}
			info->localMapSize = tmpRefFrame.getWords3().size();
			info->words = newFrame.getWords();

			info->localScanMapSize = tmpRefFrame.sensorData().laserScanRaw().size();

			info->localScanMap = util3d::transformLaserScan(tmpRefFrame.sensorData().laserScanRaw(), tmpRefFrame.sensorData().laserScanRaw().localTransform().inverse()*t*tmpRefFrame.sensorData().laserScanRaw().localTransform());
		}
	}
	else
	{
		//return Identity
		output = Transform::getIdentity();
		// a very high variance tells that the new pose is not linked with the previous one
		regInfo.covariance = cv::Mat::eye(6,6,CV_64FC1)*9999.0;
	}

	if(!output.isNull())
	{
		output = motionSinceLastKeyFrame.inverse() * output;

		// new key-frame?
		if( (registrationPipeline_->isImageRequired() &&
				(keyFrameThr_ == 0.0f ||
				 visKeyFrameThr_ == 0 ||
				 float(regInfo.inliers) <= keyFrameThr_*float(refFrame_.sensorData().keypoints().size()) ||
				 regInfo.inliers <= visKeyFrameThr_)) ||
			(registrationPipeline_->isScanRequired() && (scanKeyFrameThr_ == 0.0f || regInfo.icpInliersRatio <= scanKeyFrameThr_)))
		{
			UDEBUG("Update key frame");
			int features = newFrame.getWordsDescriptors().size();
			if(registrationPipeline_->isImageRequired() && features == 0)
			{
				newFrame = Signature(data);
				// this will generate features only for the first frame or if optical flow was used (no 3d words)
				Signature dummy;
				registrationPipeline_->computeTransformationMod(
						newFrame,
						dummy);
				features = (int)newFrame.sensorData().keypoints().size();
			}

			if((features >= registrationPipeline_->getMinVisualCorrespondences()) &&
			   (registrationPipeline_->getMinGeometryCorrespondencesRatio()==0.0f ||
					   (newFrame.sensorData().laserScanRaw().size() &&
					   (newFrame.sensorData().laserScanRaw().maxPoints() == 0 || float(newFrame.sensorData().laserScanRaw().size())/float(newFrame.sensorData().laserScanRaw().maxPoints())>=registrationPipeline_->getMinGeometryCorrespondencesRatio()))))
			{
				refFrame_ = newFrame;

				refFrame_.setWords(std::multimap<int, cv::KeyPoint>());
				refFrame_.setWords3(std::multimap<int, cv::Point3f>());
				refFrame_.setWordsDescriptors(std::multimap<int, cv::Mat>());

				//reset motion
				lastKeyFramePose_.setNull();

				addKeyFrame = true;
			}
			else
			{
				if (!refFrame_.sensorData().isValid())
				{
					// Don't send odometry if we don't have a keyframe yet
					output.setNull();
				}

				if(features < registrationPipeline_->getMinVisualCorrespondences())
				{
					UWARN("Too low 2D features (%d), keeping last key frame...", features);
				}

				if(registrationPipeline_->getMinGeometryCorrespondencesRatio()>0.0f && newFrame.sensorData().laserScanRaw().size()==0)
				{
					UWARN("Too low scan points (%d), keeping last key frame...", newFrame.sensorData().laserScanRaw().size());
				}
				else if(registrationPipeline_->getMinGeometryCorrespondencesRatio()>0.0f && newFrame.sensorData().laserScanRaw().maxPoints() != 0 && float(newFrame.sensorData().laserScanRaw().size())/float(newFrame.sensorData().laserScanRaw().maxPoints())<registrationPipeline_->getMinGeometryCorrespondencesRatio())
				{
					UWARN("Too low scan points ratio (%d < %d), keeping last key frame...", float(newFrame.sensorData().laserScanRaw().size())/float(newFrame.sensorData().laserScanRaw().maxPoints()), registrationPipeline_->getMinGeometryCorrespondencesRatio());
				}
			}
		}
	}
	else if(!regInfo.rejectedMsg.empty())
	{
		UWARN("Registration failed: \"%s\"", regInfo.rejectedMsg.c_str());
	}

	data.setFeatures(newFrame.sensorData().keypoints(), newFrame.sensorData().keypoints3D(), newFrame.sensorData().descriptors());

	if(info)
	{
		info->type = kTypeF2F;
		info->features = newFrame.sensorData().keypoints().size();
		info->keyFrameAdded = addKeyFrame;
		if(this->isInfoDataFilled())
		{
			info->reg = regInfo;
		}
		else
		{
			info->reg = regInfo.copyWithoutData();
		}
	}

	UINFO("Odom update time = %fs lost=%s inliers=%d, ref frame corners=%d, transform accepted=%s",
			timer.elapsed(),
			output.isNull()?"true":"false",
			(int)regInfo.inliers,
			(int)newFrame.sensorData().keypoints().size(),
			!output.isNull()?"true":"false");

	return output;
}

} // namespace rtabmap
