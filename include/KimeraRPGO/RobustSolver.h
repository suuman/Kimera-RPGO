/*
Backend solver class (Robust Pose Graph Optimizer)
author: Yun Chang, Luca Carlone
*/

#ifndef INCLUDE_KIMERARPGO_ROBUSTSOLVER_H_
#define INCLUDE_KIMERARPGO_ROBUSTSOLVER_H_

#include <memory>
#include <string>
#include <vector>

#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/PriorFactor.h>

#include "KimeraRPGO/GenericSolver.h"
#include "KimeraRPGO/SolverParams.h"
#include "KimeraRPGO/outlier/OutlierRemoval.h"

namespace KimeraRPGO {

/*! \brief RobustSolver type.
 *  Main backend solver that can do outlier rejection
 *  - params: RobustSolverParams, accounts for the outlier rejection method,
 * etc. (see SolverParams.h for details on RobustSolverParams)
 */
class RobustSolver : public GenericSolver {
 public:
  explicit RobustSolver(const RobustSolverParams& params);

  virtual ~RobustSolver() = default;

  size_t getNumLC() { return outlier_removal_->getNumLC(); }

  size_t getNumLCInliers() { return outlier_removal_->getNumLCInliers(); }

  /*! \brief Update call that bypasses outlier rejection.
   *  add new factors and values and optimize, without rejecting outliers.
   *  - nfg: new factors
   *  - values: linearization point for new variables
   */
  void forceUpdate(
      const gtsam::NonlinearFactorGraph& nfg = gtsam::NonlinearFactorGraph(),
      const gtsam::Values& values = gtsam::Values());

  /*! \brief Update function. Sorts through the factors, separate out the
   * odometry, the landmark measurements, and loop closures, then
   * addAndCheckIfOptimize with outlier rejection. Note that we assume the
   * odometry is ordered by key in an increasing manner
   *  - factors: the factors of the graph to be added
   *  - values: linearization point of graph to be connected
   */
  void update(const gtsam::NonlinearFactorGraph& factors,
              const gtsam::Values& values = gtsam::Values());

  /*! \brief Remove last added loop closure based on the prefixes of the robots
   * For example, to remove the last measure loop closure between robots a and c
   * removeLastLoopClosure('a', 'c');
   */
  EdgePtr removeLastLoopClosure(char prefix_1, char prefix_2);

  /*! \brief Remove last added loop closure regardless the prefixes of the
   * robots. Just remove the last added loop closure in chronological order;
   */
  EdgePtr removeLastLoopClosure();

  /*! \brief Ignore all loop closure with certian prefix
   */
  void ignorePrefix(char prefix);

  /*! \brief Revive all loop closure with certian prefix
   */
  void revivePrefix(char prefix);

  /*! \brief Get the list of ignored prefix
   */
  std::vector<char> getIgnoredPrefixes();

  /*! \brief logging data to directory indicated by path
   */
  void enableLogging(std::string path);

 private:
  std::unique_ptr<OutlierRemoval> outlier_removal_;  // outlier removal method;
  bool log_;                                         // log performance
  std::string(log_path_);

  /*! \brief Calling the optimization
   *  Optimize the factor graph with the stroed values
   *  Solver based on what was set in RobustSolverParams
   */
  void optimize();

 public:
  /*! \brief Save results from Solver
   *  Saves the resulting g2o file and also the data saved in the outlier
   * removal method.
   *  - folder_path: the directory to save the results.
   */
  void saveData(std::string folder_path) const;
};

}  // namespace KimeraRPGO

#endif  // INCLUDE_KIMERARPGO_ROBUSTSOLVER_H_