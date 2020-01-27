//
// Created by ljn on 19-8-16.
//

#include "path_optimizer/path_optimizer.hpp"
namespace PathOptimizationNS {

PathOptimizer::PathOptimizer(const std::vector<hmpl::State> &points_list,
                             const hmpl::State &start_state,
                             const hmpl::State &end_state,
                             const hmpl::InternalGridMap &map,
                             bool dnesify_path) :
    grid_map_(map),
    collision_checker_(map),
    start_state_(start_state),
    end_state_(end_state),
    car_type(ACKERMANN_STEERING),
    rear_axle_to_center_dis(1.45),
    wheel_base(2.85),
    best_sampling_index_(0),
    control_sampling_first_flag_(false),
    enable_control_sampling(true),
    densify_result(dnesify_path),
    points_list_(points_list),
    point_num_(points_list.size()),
    solver_dynamic_initialized(false) {
    setCarGeometry();
    setConfig();
}

void PathOptimizer::setConfig() {
    config_.smoothing_method_ = FRENET;
}

void PathOptimizer::setCarGeometry() {
    // Set the car geometry. Use 3 circles to approximate the car.
    // TODO: use a config file.
    double car_width = 2.0;
    double car_length = 4.9;
    // Vector car_geo is for function getClearanceWithDirection.
    // Radius of each circle.
    double circle_r = sqrt(pow(car_length / 8, 2) + pow(car_width / 2, 2));
    printf("circle r: %f\n", circle_r);
    // Distance to the vehicle center of each circle.
    double d1 = -3.0 / 8.0 * car_length;
    double d2 = -1.0 / 8.0 * car_length;
    double d3 = 1.0 / 8.0 * car_length;
    double d4 = 3.0 / 8.0 * car_length;
    double safety_margin = 0.1;
    car_geo_.emplace_back(d1);
    car_geo_.emplace_back(d2);
    car_geo_.emplace_back(d3);
    car_geo_.emplace_back(d4);
    car_geo_.emplace_back(circle_r + safety_margin);
    car_geo_.emplace_back(rear_axle_to_center_dis);
    car_geo_.emplace_back(wheel_base);
}

bool PathOptimizer::solve(std::vector<hmpl::State> *final_path) {
    auto t1 = std::clock();
    if (point_num_ == 0) {
        printf("empty input, quit path optimization\n");
        return false;
    }
    ReferencePathSmoother reference_path_smoother(points_list_, start_state_, grid_map_, config_);
    if (!reference_path_smoother.smooth(&smoothed_x_spline, &smoothed_y_spline, &smoothed_max_s, &smoothed_path_)) {
        printf("smoothing stage failed, quit path optimization.\n");
        return false;
    }
    auto t2 = std::clock();
    if (!divideSmoothedPath(true)) {
        printf("divide stage failed, quit path optimization.\n");
        return false;
    };
    auto t3 = std::clock();
    bool ok = optimizePath(final_path);
    auto t4 = std::clock();
    printf("############\n"
           "smooth phase t: %f\n"
           "divide phase t: %f\n"
           "optimize phase t: %f\n"
           "all t: %f\n"
           "############\n",
           (double) (t2 - t1) / CLOCKS_PER_SEC,
           (double) (t3 - t2) / CLOCKS_PER_SEC,
           (double) (t4 - t3) / CLOCKS_PER_SEC,
           (double) (t4 - t1) / CLOCKS_PER_SEC);
    return ok;
}

bool PathOptimizer::samplePaths(const std::vector<double> &lon_set,
                                const std::vector<double> &lat_set,
                                std::vector<std::vector<hmpl::State>> *final_path_set) {
    if (point_num_ == 0) {
        printf("empty input, quit path optimization\n");
        return false;
    }
    ReferencePathSmoother reference_path_smoother(points_list_, start_state_, grid_map_, config_);
    if (!reference_path_smoother.smooth(&smoothed_x_spline, &smoothed_y_spline, &smoothed_max_s, &smoothed_path_)) {
        printf("smoothing stage failed, quit path optimization.\n");
        return false;
    }
    if (!divideSmoothedPath(false)) {
        printf("divide path failed!\n");
        return false;
    }
    bool max_lon_flag = false;
    for (size_t i = 0; i != lon_set.size(); ++i) {
        if (i == lon_set.size() - 1) max_lon_flag = true;
        if (!sampleSingleLongitudinalPaths(lon_set[i], lat_set, final_path_set, max_lon_flag)) continue;
    }
    return !final_path_set->empty();
}

bool PathOptimizer::divideSmoothedPath(bool set_safety_margin) {
    if (smoothed_max_s == 0) {
        LOG(INFO) << "Smoothed path is empty!";
        return false;
    }
    // Calculate the initial deviation and angle difference.
    hmpl::State first_point;
    first_point.x = smoothed_x_spline(0);
    first_point.y = smoothed_y_spline(0);
    first_point.z = atan2(smoothed_y_spline.deriv(1, 0), smoothed_x_spline.deriv(1, 0));
    auto first_point_local = hmpl::globalToLocal(start_state_, first_point);
    double min_distance = hmpl::distance(start_state_, first_point);
    if (first_point_local.y < 0) {
        cte_ = min_distance;
    } else {
        cte_ = -min_distance;
    }
    epsi_ = constraintAngle(start_state_.z - first_point.z);
    // If the start heading differs a lot with the ref path, quit.
    if (fabs(epsi_) > 75 * M_PI / 180) {
        LOG(WARNING) << "initial epsi is larger than 90°, quit mpc path optimization!";
        return false;
    }
    // Divide the reference path. Intervals are smaller at the beginning.
    double delta_s_smaller = 0.3;
    // If we want to make the result path dense later, the interval here is 1.0m.
    // If we want to output the result directly, the interval is smaller.
    double delta_s_larger = densify_result ? 1.0 : 0.3;
    if (fabs(epsi_) < 20 * M_PI / 180) delta_s_smaller = delta_s_larger;
    double tmp_max_s = delta_s_smaller;
    seg_s_list_.emplace_back(0);
    while (tmp_max_s < smoothed_max_s) {
        seg_s_list_.emplace_back(tmp_max_s);
        if (tmp_max_s <= 2) {
            tmp_max_s += delta_s_smaller;
        } else {
            tmp_max_s += delta_s_larger;
        }
    }
    if (smoothed_max_s - seg_s_list_.back() > 1) {
        seg_s_list_.emplace_back(smoothed_max_s);
    }
    N_ = seg_s_list_.size();

    // Store reference states in vectors. They will be used later.
    for (size_t i = 0; i != N_; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        seg_x_list_.emplace_back(smoothed_x_spline(length_on_ref_path));
        seg_y_list_.emplace_back(smoothed_y_spline(length_on_ref_path));
        double x_d1 = smoothed_x_spline.deriv(1, length_on_ref_path);
        double y_d1 = smoothed_y_spline.deriv(1, length_on_ref_path);
        double x_d2 = smoothed_x_spline.deriv(2, length_on_ref_path);
        double y_d2 = smoothed_y_spline.deriv(2, length_on_ref_path);
        double tmp_h = atan2(y_d1, x_d1);
        double tmp_k = (x_d1 * y_d2 - y_d1 * x_d2) / pow(pow(x_d1, 2) + pow(y_d1, 2), 1.5);
        seg_angle_list_.emplace_back(tmp_h);
        seg_k_list_.emplace_back(tmp_k);
    }

    // Get clearance of covering circles.
    for (size_t i = 0; i != N_; ++i) {
        hmpl::State center_state;
        center_state.x = seg_x_list_[i];
        center_state.y = seg_y_list_[i];
        center_state.s = seg_s_list_[i];
        center_state.z = seg_angle_list_[i];
        // Function getClearance uses the center position as input.
        if (car_type == ACKERMANN_STEERING) {
            center_state.x += rear_axle_to_center_dis * cos(center_state.z);
            center_state.y += rear_axle_to_center_dis * sin(center_state.z);
        }
        std::vector<double> clearance;
        bool safety_margin_flag = set_safety_margin && seg_s_list_[i] >= 10;
        clearance = getClearanceFor4Circles(center_state, car_geo_, safety_margin_flag);
        // Terminate if collision is inevitable near the end.
        if ((clearance[0] == clearance[1] || clearance[2] == clearance[3] || clearance[4] == clearance[5]
            || clearance[6] == clearance[7])
            && center_state.s > 0.75 * smoothed_max_s) {
            printf("some states near end are not satisfying\n");
            N_ = i;
            use_end_psi = false;
            seg_x_list_.erase(seg_x_list_.begin() + i, seg_x_list_.end());
            seg_y_list_.erase(seg_y_list_.begin() + i, seg_y_list_.end());
            seg_s_list_.erase(seg_s_list_.begin() + i, seg_s_list_.end());
            seg_k_list_.erase(seg_k_list_.begin() + i, seg_k_list_.end());
            seg_angle_list_.erase(seg_angle_list_.begin() + i, seg_angle_list_.end());
            break;
        }
        seg_clearance_list_.emplace_back(clearance);
    }
    return true;
}

bool PathOptimizer::sampleSingleLongitudinalPaths(double lon,
                                                  const std::vector<double> &lat_set,
                                                  std::vector<std::vector<hmpl::State>> *final_path_set,
                                                  bool max_lon_flag) {
    auto start = std::clock();
    size_t index = 0;
    for (; index != N_; ++index) {
        if (seg_s_list_[index] > lon) break;
    }
    std::vector<double> seg_s_list, seg_angle_list, seg_k_list, seg_x_list, seg_y_list;
    std::vector<std::vector<double>> seg_clearance_list;
    seg_s_list.assign(seg_s_list_.begin(), seg_s_list_.begin() + index);
    seg_angle_list.assign(seg_angle_list_.begin(), seg_angle_list_.begin() + index);
    seg_k_list.assign(seg_k_list_.begin(), seg_k_list_.begin() + index);
    seg_clearance_list.assign(seg_clearance_list_.begin(), seg_clearance_list_.begin() + index);
    seg_x_list.assign(seg_x_list_.begin(), seg_x_list_.begin() + index);
    seg_y_list.assign(seg_y_list_.begin(), seg_y_list_.begin() + index);
    auto N = seg_s_list.size();
    auto solver_init = std::clock();
    // OSQP:
    OsqpEigen::Solver solver;
    solver.settings()->setVerbosity(false);
    solver.settings()->setWarmStart(true);
    solver.settings()->setMaxIteraction(250);
    solver.data()->setNumberOfVariables(3 * N - 1);
    solver.data()->setNumberOfConstraints(9 * N - 1);
    // Allocate QP problem matrices and vectors.
    Eigen::SparseMatrix<double> hessian;
    Eigen::VectorXd gradient = Eigen::VectorXd::Zero(3 * N - 1);
    Eigen::SparseMatrix<double> linearMatrix;
    Eigen::VectorXd lowerBound;
    Eigen::VectorXd upperBound;
    setHessianMatrix(N, &hessian);
    std::vector<double> init_state;
    init_state.emplace_back(epsi_);
    init_state.emplace_back(cte_);
    double end_angle;
//    if (max_lon_flag && use_end_psi) {
//        end_angle = end_state_.z;
//    } else {
//        end_angle = seg_angle_list.back();
//    }
    end_angle = seg_angle_list.back();
    setConstraintMatrix(N,
                        seg_s_list,
                        seg_angle_list,
                        seg_k_list,
                        seg_clearance_list,
                        &linearMatrix,
                        &lowerBound,
                        &upperBound,
                        init_state,
                        end_angle,
                        lat_set.front(),
                        0,
                        0);
//    std::streamsize prec = std::cout.precision();
//    std::cout << std::setprecision(4);
//    std::cout << "hessian:\n " << hessian << std::endl;
//    std::cout << "constraints:\n " << linearMatrix << std::endl;
//    std::cout << "lb:\n " << lowerBound << std::endl;
//    std::cout << "ub:\n " << upperBound << std::endl;
//    std::cout << std::setprecision(prec);
    // Input to solver.
    if (!solver.data()->setHessianMatrix(hessian)) return false;
    if (!solver.data()->setGradient(gradient)) return false;
    if (!solver.data()->setLinearConstraintsMatrix(linearMatrix)) return false;
    if (!solver.data()->setLowerBound(lowerBound)) return false;
    if (!solver.data()->setUpperBound(upperBound)) return false;
    if (!solver.initSolver()) return false;
    auto solving = std::clock();
    Eigen::VectorXd QPSolution;
    size_t count = 0;
    double left_bound = seg_clearance_list.back()[0];
    double right_bound = seg_clearance_list.back()[1];
//    double range = (left_bound - right_bound) * 0.95;
    double range = (left_bound - right_bound);
    double reduced_range;
    // The whole lateral sampling range is 6m.
    if (range >= 6) reduced_range = (range - 6) / 2;
    else reduced_range = 0;
    double interval = 0.3;
    std::vector<double> offset_set;
    for (size_t i = 0; i * interval <= range - 2 * reduced_range; ++i) {
        offset_set.emplace_back(right_bound + reduced_range + i * interval);
    }
    offset_set.emplace_back(0);
//    for (size_t i = 0; i != lat_set.size(); ++i) {
    hmpl::State sample_state;
    for (size_t i = 0; i != offset_set.size(); ++i) {
        sample_state.x = seg_x_list.back() + offset_set[i] * cos(seg_angle_list.back() + M_PI_2);
        sample_state.y = seg_y_list.back() + offset_set[i] * sin(seg_angle_list.back() + M_PI_2);
        sample_state.z = seg_angle_list.back();
        if (!collision_checker_.isSingleStateCollisionFreeImproved(sample_state)) {
            printf("lon: %f, lat: %f is not feasible!\n", lon, offset_set[i]);
            continue;
        }
        // Update.
        lowerBound(2 * N + 2 * N - 1) = offset_set[i] - 0.1;
        upperBound(2 * N + 2 * N - 1) = offset_set[i] + 0.1;
        if (!solver.updateBounds(lowerBound, upperBound)) break;
        // Solve.
        bool ok = solver.solve();
        if (!ok) {
            printf("solver failed at lon: %f, lat: %f!\n", lon, offset_set[i]);
            continue;
        }
        // Get single path.
        QPSolution = solver.getSolution();
        std::vector<double> result_x, result_y, result_s;
        double total_s = 0;
        double last_x, last_y;
        for (size_t j = 0; j != N; ++j) {
            double length_on_ref_path = seg_s_list[j];
            double angle = seg_angle_list[j];
            double new_angle = constraintAngle(angle + M_PI_2);
            double tmp_x = smoothed_x_spline(length_on_ref_path) + QPSolution(2 * j + 1) * cos(new_angle);
            double tmp_y = smoothed_y_spline(length_on_ref_path) + QPSolution(2 * j + 1) * sin(new_angle);
            result_x.emplace_back(tmp_x);
            result_y.emplace_back(tmp_y);
            if (j != 0) {
                total_s += sqrt(pow(tmp_x - last_x, 2) + pow(tmp_y - last_y, 2));
            }
            result_s.emplace_back(total_s);
            last_x = tmp_x;
            last_y = tmp_y;
        }
        tk::spline x_s, y_s;
        x_s.set_points(result_s, result_x);
        y_s.set_points(result_s, result_y);
        std::vector<hmpl::State> tmp_final_path;
        double delta_s = 0.3;
        double tmp_s = 0;
        hmpl::State tmp_state;
        for (int j = 0; tmp_s < result_s.back(); ++j) {
            tmp_s = j * delta_s;
            tmp_state.x = x_s(tmp_s);
            tmp_state.y = y_s(tmp_s);
            tmp_state.z = atan2(y_s.deriv(1, tmp_s), x_s.deriv(1, tmp_s));
            tmp_state.s = tmp_s;
            if (collision_checker_.isSingleStateCollisionFreeImproved(tmp_state)) {
                tmp_final_path.emplace_back(tmp_state);
            } else {
                printf("path optimization collision check failed at %f of %fm, lat: %f\n",
                       tmp_s,
                       result_s.back(),
                       offset_set[i]);
//                tmp_final_path.emplace_back(tmp_state);
                break;
            }
            if ((j + 1) * delta_s > result_s.back()) {
                tmp_s = result_s.back();
                tmp_state.x = x_s(tmp_s);
                tmp_state.y = y_s(tmp_s);
                tmp_state.z = atan2(y_s.deriv(1, tmp_s), x_s.deriv(1, tmp_s));
                tmp_state.s = tmp_s;
                if (collision_checker_.isSingleStateCollisionFreeImproved(tmp_state)) {
                    tmp_final_path.emplace_back(tmp_state);
                }
            }
        }
        if (!tmp_final_path.empty()) {
            final_path_set->emplace_back(tmp_final_path);
            ++count;
        }
    }
    auto solved = std::clock();
    printf("got %d paths at %fm\n", count, lon);
    printf("**********\n"
           "preprocess: %f\n"
           "solver init: %f\n"
           "solve: %f\n"
           "all: %f\n"
           "**********\n",
           (double) (solver_init - start) / CLOCKS_PER_SEC,
           (double) (solving - solver_init) / CLOCKS_PER_SEC,
           (double) (solved - solving) / CLOCKS_PER_SEC,
           (double) (solved - start) / CLOCKS_PER_SEC);
    return true;
}

bool PathOptimizer::optimizePath(std::vector<hmpl::State> *final_path) {
    auto po_start = std::clock();
    if (smoothed_max_s == 0) {
        LOG(INFO) << "path optimization input is empty!";
        return false;
    }
    auto po_pre = std::clock();
    // OSQP:
    // TODO: write a wrapper!
    OsqpEigen::Solver solver;
    solver.settings()->setVerbosity(false);
    solver.settings()->setWarmStart(true);
    solver.data()->setNumberOfVariables(3 * N_ - 1);
    solver.data()->setNumberOfConstraints(9 * N_ - 1);
    // Allocate QP problem matrices and vectors.
    Eigen::SparseMatrix<double> hessian;
    Eigen::VectorXd gradient = Eigen::VectorXd::Zero(3 * N_ - 1);
    Eigen::SparseMatrix<double> linearMatrix;
    Eigen::VectorXd lowerBound;
    Eigen::VectorXd upperBound;
    setHessianMatrix(N_, &hessian);
    std::vector<double> init_state;
    init_state.emplace_back(epsi_);
    init_state.emplace_back(cte_);
    bool constriant_end_psi = false;
    if (use_end_psi) constriant_end_psi = true;
    setConstraintMatrix(N_,
                        seg_s_list_,
                        seg_angle_list_,
                        seg_k_list_,
                        seg_clearance_list_,
                        &linearMatrix,
                        &lowerBound,
                        &upperBound,
                        init_state,
                        end_state_.z,
                        constriant_end_psi);
    auto po_osqp_pre = std::clock();
    // Input to solver.
    if (!solver.data()->setHessianMatrix(hessian)) return false;
    if (!solver.data()->setGradient(gradient)) return false;
    if (!solver.data()->setLinearConstraintsMatrix(linearMatrix)) return false;
    if (!solver.data()->setLowerBound(lowerBound)) return false;
    if (!solver.data()->setUpperBound(upperBound)) return false;
    // Solve.
    if (!solver.initSolver()) return false;
    if (!solver.solve()) return false;
    Eigen::VectorXd QPSolution = solver.getSolution();
    auto po_osqp_solve = std::clock();
//    std::cout << "result: " << QPSolution << std::endl;

//    // IPOPT:
//    auto po_ipopt_start = std::clock();
//    typedef CPPAD_TESTVECTOR(double) Dvector;
//    // n_vars: Set the number of model variables.
//    // There are 2 state variables: pq and psi;
//    // and 1 control variable: steer angle.
//    size_t n_vars = 2 * N + N - 1;
//    const size_t pq_range_begin = 0;
//    const size_t psi_range_begin = pq_range_begin + N;
//    const size_t steer_range_begin = psi_range_begin + N;
//    // Set the number of constraints
//    Dvector vars(n_vars);
//    for (size_t i = 0; i < n_vars; i++) {
//        vars[i] = 0;
//    }
//    vars[pq_range_begin] = cte;
//    vars[psi_range_begin] = epsi;
//    vars[steer_range_begin] = atan(start_state_.k * wheel_base);
//    // bounds of variables
//    Dvector vars_lowerbound(n_vars);
//    Dvector vars_upperbound(n_vars);
//    for (size_t i = 0; i != steer_range_begin; ++i) {
//        vars_lowerbound[i] = -DBL_MAX;
//        vars_upperbound[i] = DBL_MAX;
//    }
//    double expected_end_psi = constraintAngle(end_state_.z - seg_angle_list_.back());
//    if (original_N == N && fabs(expected_end_psi) < M_PI_2) {
//        double loose_end_psi = 10 * M_PI / 180;
//        vars_lowerbound[steer_range_begin - 1] = expected_end_psi - loose_end_psi;
//        vars_upperbound[steer_range_begin - 1] = expected_end_psi + loose_end_psi;
//    }
//    for (size_t i = steer_range_begin; i != n_vars; ++i) {
//        vars_lowerbound[i] = -30 * M_PI / 180;
//        vars_upperbound[i] = 30 * M_PI / 180;
//    }
//    vars_lowerbound[steer_range_begin] = atan(start_state_.k * wheel_base);
//    vars_upperbound[steer_range_begin] = atan(start_state_.k * wheel_base);
//    // Costraints inclued the state variable constraints(2 * N) and the covering circles constraints(4 * N).
//    // TODO: add steer change constraint.
//    size_t n_constraints = 2 * N + 4 * N;
//    Dvector constraints_lowerbound(n_constraints);
//    Dvector constraints_upperbound(n_constraints);
//    const size_t cons_pq_range_begin = 0;
//    const size_t cons_psi_range_begin = cons_pq_range_begin + N;
//    const size_t cons_c0_range_begin = cons_psi_range_begin + N;
//    const size_t cons_c1_range_begin = cons_c0_range_begin + N;
//    const size_t cons_c2_range_begin = cons_c1_range_begin + N;
//    const size_t cons_c3_range_begin = cons_c2_range_begin + N;
//    for (size_t i = cons_pq_range_begin; i != cons_c0_range_begin; ++i) {
//        constraints_upperbound[i] = 0;
//        constraints_lowerbound[i] = 0;
//    }
//    constraints_lowerbound[cons_pq_range_begin] = cte;
//    constraints_upperbound[cons_pq_range_begin] = cte;
//    constraints_lowerbound[cons_psi_range_begin] = epsi;
//    constraints_upperbound[cons_psi_range_begin] = epsi;
//    // clearance constraints for covering circles.
//    for (size_t i = 0; i != N; ++i) {
//        constraints_upperbound[cons_c0_range_begin + i] = seg_clearance_list_[i][0];
//        constraints_lowerbound[cons_c0_range_begin + i] = seg_clearance_list_[i][1];
//        constraints_upperbound[cons_c1_range_begin + i] = seg_clearance_list_[i][2];
//        constraints_lowerbound[cons_c1_range_begin + i] = seg_clearance_list_[i][3];
//        constraints_upperbound[cons_c2_range_begin + i] = seg_clearance_list_[i][4];
//        constraints_lowerbound[cons_c2_range_begin + i] = seg_clearance_list_[i][5];
//        constraints_upperbound[cons_c3_range_begin + i] = seg_clearance_list_[i][6];
//        constraints_lowerbound[cons_c3_range_begin + i] = seg_clearance_list_[i][7];
//    }
//
//    // options for IPOPT solver
//    std::string options;
//    // Uncomment this if you'd like more print information
//    options += "Integer print_level  0\n";
//    // NOTE: Setting sparse to true allows the solver to take advantage
//    // of sparse routines, this makes the computation MUCH FASTER. If you
//    // can uncomment 1 of these and see if it makes a difference or not but
//    // if you uncomment both the computation time should go up in orders of
//    // magnitude.
//    options += "Sparse  true        forward\n";
//    options += "Sparse  true        reverse\n";
//    // NOTE: Currently the solver has a maximum time limit of 0.1 seconds.
//    // Change this as you see fit.
////    options += "Numeric max_cpu_time          0.1\n";
//    /// Options for QP problem
//    options += "mehrotra_algorithm  yes\n";
//    options += "hessian_constant  yes\n";
//    options += "jac_c_constant  yes\n";
//    options += "jac_d_constant  yes\n";
//    options += "mu_strategy adaptive\n";
//
//    // place to return solution
//    CppAD::ipopt::solve_result<Dvector> solution;
//    // weights of the cost function
//    // TODO: use a config file
//    std::vector<double> weights;
//    weights.emplace_back(10); //curvature weight
//    weights.emplace_back(1000); //curvature rate weight
//    weights.emplace_back(0.01); //distance to boundary weight
//    weights.emplace_back(0.01); //offset weight
//
//    FgEvalFrenet fg_eval_frenet(seg_x_list_,
//                                seg_y_list_,
//                                seg_angle_list_,
//                                seg_k_list_,
//                                seg_s_list_,
//                                N,
//                                weights,
//                                start_state_,
//                                car_geo_,
//                                seg_clearance_list_,
//                                epsi);
//    // solve the problem
//    CppAD::ipopt::solve<Dvector, FgEvalFrenet>(options, vars,
//                                               vars_lowerbound, vars_upperbound,
//                                               constraints_lowerbound, constraints_upperbound,
//                                               fg_eval_frenet, solution);
//    // Check if it works
//    bool ok = true;
//    ok &= solution.status == CppAD::ipopt::solve_result<Dvector>::success;
//    if (!ok) {
//        LOG(WARNING) << "path optimization solver failed!";
//        return false;
//    }
//    LOG(INFO) << "path optimization solver succeeded!";
//    // output
//    for (size_t i = 0; i != N; i++) {
//        double tmp[2] = {solution.x[i], double(i)};
//        std::vector<double> v(tmp, tmp + sizeof tmp / sizeof tmp[0]);
//        this->predicted_path_in_frenet_.emplace_back(v);
////        printf("ip: %d, %f\n", i, v[0]);
//    }
//    auto po_ipopt_end = std::clock();
//    printf("ipopt time cost: %f\n", (double)(po_ipopt_end - po_ipopt_start) / CLOCKS_PER_SEC);

    std::vector<double> result_x, result_y, result_s;
    double total_s = 0;
    double last_x = 0, last_y = 0;
    std::vector<hmpl::State> tmp_raw_path;
    for (size_t i = 0; i != N_; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        double angle = seg_angle_list_[i];
        double new_angle = constraintAngle(angle + M_PI_2);
        double tmp_x = smoothed_x_spline(length_on_ref_path) + QPSolution(2 * i + 1) * cos(new_angle);
        double tmp_y = smoothed_y_spline(length_on_ref_path) + QPSolution(2 * i + 1) * sin(new_angle);
        if (!densify_result) {
            // If output raw path:
            hmpl::State state;
            state.x = tmp_x;
            state.y = tmp_y;
            state.z = angle + QPSolution(2 * i);
            if (collision_checker_.isSingleStateCollisionFreeImproved(state)) {
                tmp_raw_path.emplace_back(state);
            } else {
                tmp_raw_path.emplace_back(state);
                std::cout << "path optimization collision check failed at " << i << std::endl;
                break;
            }
        }
        result_x.emplace_back(tmp_x);
        result_y.emplace_back(tmp_y);
        if (i != 0) {
            total_s += sqrt(pow(tmp_x - last_x, 2) + pow(tmp_y - last_y, 2));
        }
        result_s.emplace_back(total_s);
        last_x = tmp_x;
        last_y = tmp_y;
    }
    std::vector<double> curvature_result;
    if (!densify_result) {
        std::copy(tmp_raw_path.begin(), tmp_raw_path.end(), std::back_inserter(*final_path));
        LOG(INFO) << "output raw path!";
        return true;
    }
    // If output interpolated path:
    tk::spline x_s, y_s;
    x_s.set_points(result_s, result_x);
    y_s.set_points(result_s, result_y);
    std::vector<hmpl::State> tmp_final_path;
    double delta_s = 0.3;
    for (int i = 0; i * delta_s <= result_s.back(); ++i) {
        hmpl::State tmp_state;
        tmp_state.x = x_s(i * delta_s);
        tmp_state.y = y_s(i * delta_s);
        tmp_state.z = atan2(y_s.deriv(1, i * delta_s), x_s.deriv(1, i * delta_s));
        tmp_state.s = i * delta_s;
        if (collision_checker_.isSingleStateCollisionFreeImproved(tmp_state)) {
            tmp_final_path.emplace_back(tmp_state);
        } else {
            std::cout << "path optimization collision check failed at " << i << std::endl;
            break;
        }
    }
    auto po_interpolation = std::clock();
    printf(
        "*****************************"
        "\ntime cost:\npre process: %f\nosqp pre process: %f\nosqp solve: %f\nB spline: %f\n"
        "*****************************\n",
        (double) (po_pre - po_start) / CLOCKS_PER_SEC,
        (double) (po_osqp_pre - po_pre) / CLOCKS_PER_SEC,
        (double) (po_osqp_solve - po_osqp_pre) / CLOCKS_PER_SEC,
        (double) (po_interpolation - po_osqp_solve) / CLOCKS_PER_SEC);
    final_path->clear();
    std::copy(tmp_final_path.begin(), tmp_final_path.end(), std::back_inserter(*final_path));
    return true;
}
bool PathOptimizer::optimizeDynamic(const std::vector<double> &sr_list,
                                    const std::vector<std::vector<double>> &clearance_list,
                                    std::vector<double> *x_list,
                                    std::vector<double> *y_list,
                                    std::vector<double> *s_list) {
    if (!solver_dynamic_initialized) {
        std::vector<double> x_set, y_set, s_set;
        for (const auto &point : points_list_) {
            x_set.emplace_back(point.x);
            y_set.emplace_back(point.y);
            s_set.emplace_back(point.s);
        }
        xsr_.set_points(s_set, x_set);
        ysr_.set_points(s_set, y_set);
        std::vector<double> angle_list, k_list;
        for (size_t i = 0; i != sr_list.size(); ++i) {
            double tmp_s = sr_list[i];
            double x_d1 = xsr_.deriv(1, tmp_s);
            double y_d1 = ysr_.deriv(1, tmp_s);
            double x_d2 = xsr_.deriv(2, tmp_s);
            double y_d2 = ysr_.deriv(2, tmp_s);
            double h = atan2(y_d1, x_d1);
            double k = (x_d1 * y_d2 - y_d1 * x_d2) / pow(pow(x_d1, 2) + pow(y_d1, 2), 1.5);
            angle_list.emplace_back(h);
            k_list.emplace_back(k);
        }
        auto N = sr_list.size();
        solver_dynamic.settings()->setVerbosity(false);
        solver_dynamic.settings()->setWarmStart(true);
//        solver_dynamic.settings()->setMaxIteraction(250);
        solver_dynamic.data()->setNumberOfVariables(3 * N - 1);
        solver_dynamic.data()->setNumberOfConstraints(9 * N - 1);
        // Allocate QP problem matrices and vectors.
        Eigen::SparseMatrix<double> hessian;
        Eigen::VectorXd gradient = Eigen::VectorXd::Zero(3 * N - 1);
        Eigen::SparseMatrix<double> linearMatrix;
        setHessianMatrix(N, &hessian);
        std::vector<double> init_state;
        init_state.emplace_back(0);
        init_state.emplace_back(0);
        std::vector<std::vector<double>> fuck;
        setConstraintMatrix(N,
                            sr_list,
                            angle_list,
                            k_list,
                            clearance_list,
                            &linearMatrix,
                            &lower_bound_dynamic_,
                            &upper_bound_dynamic_,
                            init_state,
                            end_state_.z,
                            true);
        if (!solver_dynamic.data()->setHessianMatrix(hessian)) return false;
        if (!solver_dynamic.data()->setGradient(gradient)) return false;
        if (!solver_dynamic.data()->setLinearConstraintsMatrix(linearMatrix)) return false;
        if (!solver_dynamic.data()->setLowerBound(lower_bound_dynamic_)) return false;
        if (!solver_dynamic.data()->setUpperBound(upper_bound_dynamic_)) return false;
        if (!solver_dynamic.initSolver()) return false;
        solver_dynamic_initialized = true;
        bool ok = solver_dynamic.solve();
        if (!ok) {
            printf("dynamic solver failed\n");
            return false;
        }
        auto QPSolution = solver_dynamic.getSolution();
        double total_s = 0;
        for (size_t j = 0; j != N; ++j) {
            double length_on_ref_path = sr_list[j];
            double angle = angle_list[j];
            double new_angle = constraintAngle(angle + M_PI_2);
            double tmp_x = xsr_(length_on_ref_path) + QPSolution(2 * j + 1) * cos(new_angle);
            double tmp_y = ysr_(length_on_ref_path) + QPSolution(2 * j + 1) * sin(new_angle);
            if (j != 0) {
                total_s += sqrt(pow(tmp_x - x_list->back(), 2) + pow(tmp_y - y_list->back(), 2));
            }
            s_list->emplace_back(total_s);
            x_list->emplace_back(tmp_x);
            y_list->emplace_back(tmp_y);
        }
        return true;
    } else {
        auto N = sr_list.size();
        for (size_t i = 0; i != N; ++i) {
            Eigen::Vector4d ld, ud;
            ud
                << clearance_list[i][0], clearance_list[i][2], clearance_list[i][4], clearance_list[i][6];
            ld
                << clearance_list[i][1], clearance_list[i][3], clearance_list[i][5], clearance_list[i][7];
            lower_bound_dynamic_.block(5 * N - 1 + 4 * i, 0, 4, 1) = ld;
            upper_bound_dynamic_.block(5 * N - 1 + 4 * i, 0, 4, 1) = ud;
        }
        if (!solver_dynamic.updateBounds(lower_bound_dynamic_, upper_bound_dynamic_)) return false;
        bool ok = solver_dynamic.solve();
        if (!ok) {
            printf("dynamic solver failed\n");
            return false;
        }
        auto QPSolution = solver_dynamic.getSolution();
        double total_s = 0;
        for (size_t j = 0; j != N; ++j) {
            double length_on_ref_path = sr_list[j];
            double x_d1 = xsr_.deriv(1, length_on_ref_path);
            double y_d1 = ysr_.deriv(1, length_on_ref_path);
            double angle = atan2(y_d1, x_d1);
            double new_angle = constraintAngle(angle + M_PI_2);
            double tmp_x = xsr_(length_on_ref_path) + QPSolution(2 * j + 1) * cos(new_angle);
            double tmp_y = ysr_(length_on_ref_path) + QPSolution(2 * j + 1) * sin(new_angle);
            if (j != 0) {
                total_s += sqrt(pow(tmp_x - x_list->back(), 2) + pow(tmp_y - y_list->back(), 2));
            }
            s_list->emplace_back(total_s);
            x_list->emplace_back(tmp_x);
            y_list->emplace_back(tmp_y);
        }
        return true;
    }

}

std::vector<double> PathOptimizer::getClearanceWithDirectionStrict(hmpl::State state,
                                                                   double radius,
                                                                   bool safety_margin_flag) const {
    double left_bound = 0;
    double right_bound = 0;
    double delta_s = 0.2;
    double left_angle = constraintAngle(state.z + M_PI_2);
    double right_angle = constraintAngle(state.z - M_PI_2);
    auto n = static_cast<size_t >(5.0 / delta_s);
    // Check if the original position is collision free.
    grid_map::Position original_position(state.x, state.y);
    auto original_clearance = grid_map_.getObstacleDistance(original_position);
    if (original_clearance > radius) {
        // Normal case:
        double right_s = 0;
        for (size_t j = 0; j != n; ++j) {
            right_s += delta_s;
            double x = state.x + right_s * cos(right_angle);
            double y = state.y + right_s * sin(right_angle);
            grid_map::Position new_position(x, y);
            double clearance = grid_map_.getObstacleDistance(new_position);
            if (clearance < radius) {
                break;
            }
        }
        double left_s = 0;
        for (size_t j = 0; j != n; ++j) {
            left_s += delta_s;
            double x = state.x + left_s * cos(left_angle);
            double y = state.y + left_s * sin(left_angle);
            grid_map::Position new_position(x, y);
            double clearance = grid_map_.getObstacleDistance(new_position);
            if (clearance < radius) {
                break;
            }
        }
        right_bound = -(right_s - delta_s);
        left_bound = left_s - delta_s;
    } else {
        // Collision already; pick one side to expand:
        double right_s = 0;
        for (size_t j = 0; j != n / 2; ++j) {
            right_s += delta_s;
            double x = state.x + right_s * cos(right_angle);
            double y = state.y + right_s * sin(right_angle);
            grid_map::Position new_position(x, y);
            double clearance = grid_map_.getObstacleDistance(new_position);
            if (clearance > radius) {
                break;
            }
        }
        double left_s = 0;
        for (size_t j = 0; j != n / 2; ++j) {
            left_s += delta_s;
            double x = state.x + left_s * cos(left_angle);
            double y = state.y + left_s * sin(left_angle);
            grid_map::Position new_position(x, y);
            double clearance = grid_map_.getObstacleDistance(new_position);
            if (clearance > radius) {
                break;
            }
        }
        // Compare
        if (left_s < right_s) {
            // Pick left side:
            right_bound = left_s;
            for (size_t j = 0; j != n; ++j) {
                left_s += delta_s;
                double x = state.x + left_s * cos(left_angle);
                double y = state.y + left_s * sin(left_angle);
                grid_map::Position new_position(x, y);
                double clearance = grid_map_.getObstacleDistance(new_position);
                if (clearance < radius) {
                    break;
                }
            }
            left_bound = left_s - delta_s;
        } else {
            // Pick right side:
            left_bound = -right_s;
            for (size_t j = 0; j != n; ++j) {
                right_s += delta_s;
                double x = state.x + right_s * cos(right_angle);
                double y = state.y + right_s * sin(right_angle);
                grid_map::Position new_position(x, y);
                double clearance = grid_map_.getObstacleDistance(new_position);
                if (clearance < radius) {
                    break;
                }
            }
            right_bound = -(right_s - delta_s);
        }
    }
    double base = std::max(left_bound - right_bound - 0.6, 0.0);
    if (safety_margin_flag) {
        double safety_margin = std::min(base * 0.2, 0.5);
        left_bound -= safety_margin;
        right_bound += safety_margin;
    }
    std::vector<double> result{left_bound, right_bound};
    return result;
}

std::vector<double> PathOptimizer::getClearanceFor4Circles(const hmpl::State &state,
                                                           const std::vector<double> &car_geometry,
                                                           bool safety_margin_flag){
    double circle_r = car_geometry[4];
    hmpl::State c0, c1, c2, c3;
    double center_x = state.x;
    double center_y = state.y;
    double c0_x = center_x + car_geometry[0] * cos(state.z);
    double c0_y = center_y + car_geometry[0] * sin(state.z);
    double c1_x = center_x + car_geometry[1] * cos(state.z);
    double c1_y = center_y + car_geometry[1] * sin(state.z);
    double c2_x = center_x + car_geometry[2] * cos(state.z);
    double c2_y = center_y + car_geometry[2] * sin(state.z);
    double c3_x = center_x + car_geometry[3] * cos(state.z);
    double c3_y = center_y + car_geometry[3] * sin(state.z);
    c0.x = c0_x;
    c0.y = c0_y;
    c0.z = state.z;
    c1.x = c1_x;
    c1.y = c1_y;
    c1.z = state.z;
    c2.x = c2_x;
    c2.y = c2_y;
    c2.z = state.z;
    c3.x = c3_x;
    c3.y = c3_y;
    c3.z = state.z;
    std::vector<double> result;
    std::vector<double> c0_bounds = getClearanceWithDirectionStrict(c0, circle_r, safety_margin_flag);
    std::vector<double> c1_bounds = getClearanceWithDirectionStrict(c1, circle_r, safety_margin_flag);
    std::vector<double> c2_bounds = getClearanceWithDirectionStrict(c2, circle_r, safety_margin_flag);
    std::vector<double> c3_bounds = getClearanceWithDirectionStrict(c3, circle_r, safety_margin_flag);
    result.emplace_back(c0_bounds[0]);
    result.emplace_back(c0_bounds[1]);
    result.emplace_back(c1_bounds[0]);
    result.emplace_back(c1_bounds[1]);
    result.emplace_back(c2_bounds[0]);
    result.emplace_back(c2_bounds[1]);
    result.emplace_back(c3_bounds[0]);
    result.emplace_back(c3_bounds[1]);
    // For visualization purpoose:
    hmpl::State rear_bound_l, center_bound_l, front_bound_l, rear_bound_r, center_bound_r, front_bound_r;
    rear_bound_l.x = c0_x + result[0] * cos(state.z + M_PI_2);
    rear_bound_l.y = c0_y + result[0] * sin(state.z + M_PI_2);
    rear_bound_r.x = c0_x + result[1] * cos(state.z + M_PI_2);
    rear_bound_r.y = c0_y + result[1] * sin(state.z + M_PI_2);
    center_bound_l.x = c2_x + result[4] * cos(state.z + M_PI_2);
    center_bound_l.y = c2_y + result[4] * sin(state.z + M_PI_2);
    center_bound_r.x = c2_x + result[5] * cos(state.z + M_PI_2);
    center_bound_r.y = c2_y + result[5] * sin(state.z + M_PI_2);
    front_bound_l.x = c3_x + result[6] * cos(state.z + M_PI_2);
    front_bound_l.y = c3_y + result[6] * sin(state.z + M_PI_2);
    front_bound_r.x = c3_x + result[7] * cos(state.z + M_PI_2);
    front_bound_r.y = c3_y + result[7] * sin(state.z + M_PI_2);
    rear_bounds_.emplace_back(rear_bound_l);
    rear_bounds_.emplace_back(rear_bound_r);
    center_bounds_.emplace_back(center_bound_l);
    center_bounds_.emplace_back(center_bound_r);
    front_bounds_.emplace_back(front_bound_l);
    front_bounds_.emplace_back(front_bound_r);
    return result;

}

const std::vector<std::vector<hmpl::State> > &PathOptimizer::getControlSamplingPathSet() const {
    return this->control_sampling_path_set_;
};

const std::vector<std::vector<hmpl::State> > &PathOptimizer::getControlSamplingFailedPathSet() const {
    return this->failed_sampling_path_set_;
};

const std::vector<hmpl::State> &PathOptimizer::getBestSamplingPath() const {
    if (control_sampling_first_flag_) {
        return this->control_sampling_path_set_[best_sampling_index_];
    } else {
        return this->empty_;
    }
}

const std::vector<hmpl::State> &PathOptimizer::getLeftBound() const {
    return this->left_bound_;
}

const std::vector<hmpl::State> &PathOptimizer::getRightBound() const {
    return this->right_bound_;
}

const std::vector<hmpl::State> &PathOptimizer::getSecondThirdPoint() const {
    return this->second_third_point_;
}

const std::vector<hmpl::State> &PathOptimizer::getRearBounds() const {
    return this->rear_bounds_;
}

const std::vector<hmpl::State> &PathOptimizer::getCenterBounds() const {
    return this->center_bounds_;
}

const std::vector<hmpl::State> &PathOptimizer::getFrontBounds() const {
    return this->front_bounds_;
}

const std::vector<hmpl::State> &PathOptimizer::getSmoothedPath() const {
    return this->smoothed_path_;
}
}
