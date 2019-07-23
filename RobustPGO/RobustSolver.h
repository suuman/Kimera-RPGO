/*
Backend solver class (Robust Pose Graph Optimizer)
author: Yun Chang, Luca Carlone
*/

#ifndef ROBUSTSOLVER_H
#define ROBUSTSOLVER_H

#include <gtsam/base/Vector.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/Rot3.h>
#include <gtsam/linear/NoiseModel.h>
#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam/nonlinear/GaussNewtonOptimizer.h>
#include <gtsam/nonlinear/DoglegOptimizer.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/slam/InitializePose3.h>
#include <gtsam/nonlinear/NonlinearConjugateGradientOptimizer.h>
#include <gtsam/inference/Symbol.h>

#include "RobustPGO/GenericSolver.h"
#include "RobustPGO/logger.h"
#include "RobustPGO/outlier/OutlierRemoval.h"
#include "RobustPGO/SolverParams.h"
#include "RobustPGO/outlier/pcm.h"

namespace RobustPGO {

/*! \brief RobustSolver type.
   *  Main backend solver that can do outlier rejection
   *  - params: RobustSolverPrams, accounts for the outlier rejection method, etc.
   */
class RobustSolver : public GenericSolver{
public:
  RobustSolver(const RobustSolverParams& params);
  // solvertype = 1 for LevenbergMarquardt, 2 for GaussNewton // TODO(Luca): this seems an old comment

  /*! \brief Normal update call for Robust Solver
   *  add new factors and values and optimize, possibly after rejecting outliers.
   *  - nfg: new factors
   *  - values: linearization point for new variables
   */
  void update(const gtsam::NonlinearFactorGraph& nfg=gtsam::NonlinearFactorGraph(),
              const gtsam::Values& values=gtsam::Values());

  /*! \brief Update call that bypasses outlier rejection.
   *  add new factors and values and optimize, without rejecting outliers.
   *  - nfg: new factors
   *  - values: linearization point for new variables
   */
  void forceUpdate(const gtsam::NonlinearFactorGraph& nfg=gtsam::NonlinearFactorGraph(),
              const gtsam::Values& values=gtsam::Values());

  /*! \brief Load a factor graph with a prior
   *  Assuming a newly initialized RobustSolver. Use load graph to load a whole factor graph
   *  - factors: the nonlinear factor graph to be loaded
   *  - values: linearization point for the loaded variables
   *  - prior: the prior factor anchoring the graph, associated with the first value (lowest key ex. 0, or a0, etc. )
   */
  template<class T>
  void loadGraph(const gtsam::NonlinearFactorGraph& factors, const gtsam::Values& values,
      const gtsam::PriorFactor<T>& prior) {
    gtsam::NonlinearFactorGraph prior_factor;
    gtsam::Values prior_values;
    prior_factor.add(prior);
    prior_values.insert(prior.key(), prior.prior());

    if (outlier_removal_) {
      outlier_removal_->process(prior_factor, prior_values, nfg_, values_);
    } else {
      process(prior_factor, prior_values);
    }

    connectGraph<T>(factors, values, prior.key());

    optimize(); // optimize once after loading
  }

  /*! \brief Load a factor graph without a prior
   *  Assuming a newly initialized RobustSolver. Use load graph to load a whole factor graph
   *  - factors: the nonlinear factor graph to be loaded
   *  - values: linearization point for the loaded variables
   *  - key0: the key associated with the first value (lowest key ex. 0, or a0, etc. )
   */
  template<class T>
  void loadGraph(gtsam::NonlinearFactorGraph factors, gtsam::Values values, gtsam::Key key0=0) {
    gtsam::Values prior_values;
    prior_values.insert(key0, values.at<T>(key0));

    if (outlier_removal_) {
      outlier_removal_->process(gtsam::NonlinearFactorGraph(), prior_values, nfg_, values_);
    } else {
      process(gtsam::NonlinearFactorGraph(), prior_values);
    }

    connectGraph<T>(factors, values, key0);

    optimize(); // optimize once after loading
  }

  /*! \brief Add a factor graph to solver with a between factor for connection
   *  Assuming a RobustSolver that already has some existing information. Use add graph to
   *  add another whole factor graph to the existing.
   *  - factors: the nonlinear factor graph to be added
   *  - values: linearization point for the added variables
   *  - connector: the BetweenFactor associated that attaches the new graph to some point
   *    in the exisitng graph.
   */
  template<class T>
  void addGraph(const gtsam::NonlinearFactorGraph& factors, const gtsam::Values& values,
      const gtsam::BetweenFactor<T>& connector) {

    gtsam::Key key0 = connector.back();

    gtsam::NonlinearFactorGraph connect_factor;
    gtsam::Values connect_values;
    connect_factor.add(connector);
    connect_values.insert(key0, values.at<T>(key0));

    if (outlier_removal_) {
      outlier_removal_->process(connect_factor, connect_values, nfg_, values_);
    } else {
      process(connect_factor, connect_values);
    }

    connectGraph<T>(factors, values, key0);

    optimize(); // optimize once after loading
  }

  /*! \brief Save results from Solver
   *  Saves the resulting g2o file and also the data saved in the outlier removal method.
   *  - folder_path: the directory to save the results.
   */
  void saveData(std::string folder_path) const {
    std::string g2o_file_path = folder_path + "/result.g2o";
    gtsam::writeG2o(nfg_, values_, g2o_file_path);
    outlier_removal_->saveData(folder_path);
  }

private:
  /*! \brief Used with Add and Load to connect factor graph to between or prior factor
   *  Sorts through the factors, separate out the odometry, the landmark measurements,
   *  and loop closures, then process with outlier rejection
   *  - factors: the factors of the graph to be added
   *  - values: linearization point of graph to be connected
   *  - key0: Lowest key of the graph to be connected (root of odometry)
   */
  template<class T>
  void connectGraph(gtsam::NonlinearFactorGraph factors,
      const gtsam::Values& values, const gtsam::Key& key0) {

    // load graph assumes that the previous graph has been cleared
    gtsam::Key current_key = key0; // initial key
    // note that as of now only deal with between factors)
    // first load the odometry
    // order a nonlinear factor graph as odometry first
    bool extracted_odom = false;
    while (!extracted_odom) {
      bool end_of_odom = true;
      for (size_t i = 0; i < factors.size(); i++) {
        // search through
        if (factors[i] != NULL &&
            factors[i]->front() == current_key && factors[i]->back() == current_key + 1) {
          end_of_odom = false;
          // if (debug_) {
          //     std::cout << "odometry: " << current_key << ">" << current_key + 1 << std::endl;
          // }
          gtsam::Values new_values;
          gtsam::NonlinearFactorGraph new_factors;
          // TODO(Luca): why renaming the keys?
          new_values.insert(current_key + 1, values.at<T>(current_key + 1));
          new_factors.add(factors[i]);

          if (outlier_removal_) {
            outlier_removal_->process(new_factors, new_values, nfg_, values_);
          } else {
            process(new_factors, new_values);
          }

          current_key = current_key + 1;
          factors[i].reset();
          break;
        }
      }
      if (end_of_odom) extracted_odom = true;
    }

    // now search for the special symbols (i.e. artifacts)
    for (size_t i = 0; i < factors.size(); i++) {
      if (factors[i] != NULL){
        gtsam::Symbol symb(factors[i]->back());
        if (isSpecialSymbol(symb.chr())) {
          gtsam::Values new_values;
          gtsam::NonlinearFactorGraph new_factors;
          new_values.insert(factors[i]->back(), values.at<T>(factors[i]->back()));
          new_factors.add(factors[i]);

          if (outlier_removal_) {
            outlier_removal_->process(new_factors, new_values, nfg_, values_);
          } else {
            process(new_factors, new_values);
          }

          factors[i].reset();
          break;
        }
      }
    }

    // add the other non-odom loop closures
    gtsam::NonlinearFactorGraph new_factors;
    for (size_t i = 0; i < factors.size(); i++) {
      if (factors[i] != NULL) {
        // if (debug_) {
        //   std::cout << "loop closure: " << factors[i]->front() << ">" << factors[i]->back() << std::endl;
        // }
        new_factors.add(factors[i]);
      }
    }

    if (outlier_removal_) {
      outlier_removal_->process(new_factors, gtsam::Values(), nfg_, values_);
    } else {
      process(new_factors, gtsam::Values());
    }

    optimize();
  }

private:
  std::unique_ptr<OutlierRemoval> outlier_removal_; // outlier removal method;

  /*! \brief Calling the optimization
   *  Optimize the factor graph with the stroed values
   *  Solver based on what was set in RobustSolverParams
   */
  void optimize();
};

}
#endif
