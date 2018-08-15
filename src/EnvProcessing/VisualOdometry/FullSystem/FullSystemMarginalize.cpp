/**
 *
 * ____             _        _____           _
 *|  _ \           (_)      / ____|         | |
 *| |_) | ___  _ __ _ ___  | (___  _   _ ___| |_ ___ _ __ ___
 *|  _ < / _ \| '__| / __|  \___ \| | | / __| __/ _ \ '_ ` _ \
 *| |_) | (_) | |  | \__ \  ____) | |_| \__ \ ||  __/ | | | | |
 *|____/ \___/|_|  |_|___/ |_____/ \__, |___/\__\___|_| |_| |_| 2018
 *                                  __/ |
 *                                 |___/
 *
 *                     © Charles200000
*/

/**
* Copyright 2016 Technical University of Munich and Intel.
* Developed by Jakob Engel
*/



/*
 * KFBuffer.cpp
 *
 *  Created on: Jan 7, 2014
 *      Author: engelj
 */

#include "EnvProcessing/VisualOdometry/FullSystem/FullSystem.h"
 
#include "stdio.h"
#include "EnvProcessing/EP_Utils/globalFuncs.h"
#include <Eigen/LU>
#include <algorithm>
#include "EnvProcessing/IOWrapper/ImageDisplay.h"
#include "EnvProcessing/EP_Utils/globalCalib.h"

#include <Eigen/SVD>
#include <Eigen/Eigenvalues>
#include "EnvProcessing/VisualOdometry/FullSystem/ResidualProjections.h"
#include "EnvProcessing/VisualOdometry/FullSystem/ImmaturePoint.h"

#include "EnvProcessing/VisualOdometry/OptimizationBackend/EnergyFunctional.h"
#include "EnvProcessing/VisualOdometry/OptimizationBackend/EnergyFunctionalStructs.h"

#include "EnvProcessing/IOWrapper/Output3DWrapper.h"

#include "EnvProcessing/VisualOdometry/FullSystem/CoarseTracker.h"


namespace Boris_Brain {
	namespace dso {


		void FullSystem::flagFramesForMarginalization(FrameHessian *newFH) {
			if (setting_minFrameAge > setting_maxFrames) {
				for (int i = setting_maxFrames; i < (int) frameHessians.size(); i++) {
					FrameHessian *fh = frameHessians[i - setting_maxFrames];
					fh->flaggedForMarginalization = true;
				}
				return;
			}


			int flagged = 0;
			// marginalize all frames that have not enough points.
			for (int i = 0; i < (int) frameHessians.size(); i++) {
				FrameHessian *fh = frameHessians[i];
				int in = fh->pointHessians.size() + fh->immaturePoints.size();
				int out = fh->pointHessiansMarginalized.size() + fh->pointHessiansOut.size();


				Vec2 refToFh = AffLight::fromToVecExposure(frameHessians.back()->ab_exposure, fh->ab_exposure,
														   frameHessians.back()->aff_g2l(), fh->aff_g2l());


				if ((in < setting_minPointsRemaining * (in + out) ||
					 fabs(logf((float) refToFh[0])) > setting_maxLogAffFacInWindow)
					&& ((int) frameHessians.size()) - flagged > setting_minFrames) {
//			printf("MARGINALIZE frame %d, as only %'d/%'d points remaining (%'d %'d %'d %'d). VisInLast %'d / %'d. traces %d, activated %d!\n",
//					fh->frameID, in, in+out,
//					(int)fh->pointHessians.size(), (int)fh->immaturePoints.size(),
//					(int)fh->pointHessiansMarginalized.size(), (int)fh->pointHessiansOut.size(),
//					visInLast, outInLast,
//					fh->statistics_tracesCreatedForThisFrame, fh->statistics_pointsActivatedForThisFrame);
					fh->flaggedForMarginalization = true;
					flagged++;
				} else {
//			printf("May Keep frame %d, as %'d/%'d points remaining (%'d %'d %'d %'d). VisInLast %'d / %'d. traces %d, activated %d!\n",
//					fh->frameID, in, in+out,
//					(int)fh->pointHessians.size(), (int)fh->immaturePoints.size(),
//					(int)fh->pointHessiansMarginalized.size(), (int)fh->pointHessiansOut.size(),
//					visInLast, outInLast,
//					fh->statistics_tracesCreatedForThisFrame, fh->statistics_pointsActivatedForThisFrame);
				}
			}

			// marginalize one.
			if ((int) frameHessians.size() - flagged >= setting_maxFrames) {
				double smallestScore = 1;
				FrameHessian *toMarginalize = 0;
				FrameHessian *latest = frameHessians.back();


				for (FrameHessian *fh : frameHessians) {
					if (fh->frameID > latest->frameID - setting_minFrameAge || fh->frameID == 0) continue;
					//if(fh==frameHessians.front() == 0) continue;

					double distScore = 0;
					for (FrameFramePrecalc &ffh : fh->targetPrecalc) {
						if (ffh.target->frameID > latest->frameID - setting_minFrameAge + 1 ||
							ffh.target == ffh.host)
							continue;
						distScore += 1 / (1e-5 + ffh.distanceLL);

					}
					distScore *= -sqrtf(fh->targetPrecalc.back().distanceLL);


					if (distScore < smallestScore) {
						smallestScore = distScore;
						toMarginalize = fh;
					}
				}

//		printf("MARGINALIZE frame %d, as it is the closest (score %.2f)!\n",
//				toMarginalize->frameID, smallestScore);
				toMarginalize->flaggedForMarginalization = true;
				flagged++;
			}

//	printf("FRAMES LEFT: ");
//	for(FrameHessian* fh : frameHessians)
//		printf("%d ", fh->frameID);
//	printf("\n");
		}


		void FullSystem::marginalizeFrame(FrameHessian *frame) {
			// marginalize or remove all this frames points.

			assert((int) frame->pointHessians.size() == 0);


			ef->marginalizeFrame(frame->efFrame);

			// drop all observations of existing points in that frame.

			for (FrameHessian *fh : frameHessians) {
				if (fh == frame) continue;

				for (PointHessian *ph : fh->pointHessians) {
					for (unsigned int i = 0; i < ph->residuals.size(); i++) {
						PointFrameResidual *r = ph->residuals[i];
						if (r->target == frame) {
							if (ph->lastResiduals[0].first == r)
								ph->lastResiduals[0].first = 0;
							else if (ph->lastResiduals[1].first == r)
								ph->lastResiduals[1].first = 0;


							if (r->host->frameID < r->target->frameID)
								statistics_numForceDroppedResFwd++;
							else
								statistics_numForceDroppedResBwd++;

							ef->dropResidual(r->efResidual);
							deleteOut<PointFrameResidual>(ph->residuals, i);
							break;
						}
					}
				}
			}


			{
				std::vector<FrameHessian *> v;
				v.push_back(frame);
				for (IOWrap::Output3DWrapper *ow : outputWrapper)
					ow->publishKeyframes(v, true, &Hcalib);
			}


			frame->shell->marginalizedAt = frameHessians.back()->shell->id;
			frame->shell->movedByOpt = frame->w2c_leftEps().norm();

			deleteOutOrder<FrameHessian>(frameHessians, frame);
			for (unsigned int i = 0; i < frameHessians.size(); i++)
				frameHessians[i]->idx = i;


			setPrecalcValues();
			ef->setAdjointsF(&Hcalib);
		}
	}
}