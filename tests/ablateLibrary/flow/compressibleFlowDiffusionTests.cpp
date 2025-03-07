#include <petsc.h>
#include <cmath>
#include <flow/processes/eulerDiffusion.hpp>
#include <memory>
#include <mesh/dmWrapper.hpp>
#include <vector>
#include "MpiTestFixture.hpp"
#include "PetscTestErrorChecker.hpp"
#include "eos/perfectGas.hpp"
#include "eos/transport/constant.hpp"
#include "flow/boundaryConditions/ghost.hpp"
#include "flow/compressibleFlow.hpp"
#include "flow/fluxCalculator/offFlux.hpp"
#include "flow/processes/eulerAdvection.hpp"
#include "gtest/gtest.h"
#include "mathFunctions/functionFactory.hpp"
#include "parameters/mapParameters.hpp"

typedef struct {
    PetscInt dim;
    PetscReal L;
    PetscReal gamma;
    PetscReal Rgas;
    PetscReal k;
    PetscReal rho;
    PetscReal Tinit;
    PetscReal Tboundary;
} InputParameters;

struct CompressibleFlowDiffusionTestParameters {
    testingResources::MpiTestParameter mpiTestParameter;
    InputParameters parameters;
    PetscInt initialNx;
    int levels;
    std::vector<PetscReal> expectedL2Convergence;
    std::vector<PetscReal> expectedLInfConvergence;
};

using namespace ablate;

class CompressibleFlowDiffusionTestFixture : public testingResources::MpiTestFixture, public ::testing::WithParamInterface<CompressibleFlowDiffusionTestParameters> {
   public:
    void SetUp() override { SetMpiParameters(GetParam().mpiTestParameter); }
};

/**
 * Simple function to compute the exact solution for a given xyz and time
 */
static PetscReal ComputeTExact(PetscReal time, const PetscReal xyz[], InputParameters *parameters) {
    // compute cv for a perfect gas
    PetscReal cv = parameters->gamma * parameters->Rgas / (parameters->gamma - 1) - parameters->Rgas;

    // compute the alpha in the equation
    PetscReal alpha = parameters->k / (parameters->rho * cv);
    PetscReal Tinitial = (parameters->Tinit - parameters->Tboundary);
    PetscReal T = 0.0;
    for (PetscReal n = 1; n < 2000; n++) {
        PetscReal Bn = -Tinitial * 2.0 * (-1.0 + PetscPowReal(-1.0, n)) / (n * PETSC_PI);
        T += Bn * PetscSinReal(n * PETSC_PI * xyz[0] / parameters->L) * PetscExpReal(-n * n * PETSC_PI * PETSC_PI * alpha * time / (PetscSqr(parameters->L)));
    }

    return T + parameters->Tboundary;
}

static PetscErrorCode EulerExact(PetscInt dim, PetscReal time, const PetscReal xyz[], PetscInt Nf, PetscScalar *node, void *ctx) {
    PetscFunctionBeginUser;

    InputParameters *parameters = (InputParameters *)ctx;

    PetscReal T = ComputeTExact(time, xyz, parameters);

    PetscReal u = 0.0;
    PetscReal v = 0.0;
    PetscReal p = parameters->rho * parameters->Rgas * T;
    PetscReal e = p / ((parameters->gamma - 1.0) * parameters->rho);
    PetscReal eT = e + 0.5 * (u * u + v * v);

    node[ablate::flow::processes::EulerAdvection::RHO] = parameters->rho;
    node[ablate::flow::processes::EulerAdvection::RHOE] = parameters->rho * eT;
    node[ablate::flow::processes::EulerAdvection::RHOU + 0] = parameters->rho * u;
    node[ablate::flow::processes::EulerAdvection::RHOU + 1] = parameters->rho * v;

    PetscFunctionReturn(0);
}

static PetscErrorCode PhysicsBoundary_Euler(PetscReal time, const PetscReal *c, const PetscReal *n, const PetscScalar *a_xI, PetscScalar *a_xG, void *ctx) {
    PetscFunctionBeginUser;
    InputParameters *parameters = (InputParameters *)ctx;

    PetscReal T = parameters->Tboundary;
    PetscReal u = 0.0;
    PetscReal v = 0.0;
    PetscReal p = parameters->rho * parameters->Rgas * T;
    PetscReal e = p / ((parameters->gamma - 1.0) * parameters->rho);
    PetscReal eT = e + 0.5 * (u * u + v * v);

    a_xG[ablate::flow::processes::EulerAdvection::RHO] = parameters->rho;
    a_xG[ablate::flow::processes::EulerAdvection::RHOE] = parameters->rho * eT;
    a_xG[ablate::flow::processes::EulerAdvection::RHOU + 0] = parameters->rho * u;
    a_xG[ablate::flow::processes::EulerAdvection::RHOU + 1] = parameters->rho * v;

    PetscFunctionReturn(0);
}

static PetscErrorCode PhysicsBoundary_Mirror(PetscReal time, const PetscReal *c, const PetscReal *n, const PetscScalar *a_xI, PetscScalar *a_xG, void *ctx) {
    PetscFunctionBeginUser;
    InputParameters *constants = (InputParameters *)ctx;

    // Offset the calc assuming the cells are square
    for (PetscInt f = 0; f < ablate::flow::processes::EulerAdvection::RHOU + constants->dim; f++) {
        a_xG[f] = a_xI[f];
    }
    PetscFunctionReturn(0);
}

static void ComputeErrorNorms(TS ts, std::shared_ptr<ablate::flow::CompressibleFlow> flowData, std::vector<PetscReal> &residualNorm2, std::vector<PetscReal> &residualNormInf,
                              InputParameters *parameters, PetscTestErrorChecker &errorChecker) {
    // Compute the error
    PetscDS ds;
    DMGetDS(flowData->GetDM(), &ds) >> errorChecker;

    // Get the current time
    PetscReal time;
    TSGetTime(ts, &time) >> errorChecker;

    // Get the exact solution
    void *exactCtxs[1];
    PetscErrorCode (*exactFuncs[1])(PetscInt dim, PetscReal time, const PetscReal x[], PetscInt Nf, PetscScalar *u, void *ctx);
    PetscDSGetExactSolution(ds, 0, &exactFuncs[0], &exactCtxs[0]) >> errorChecker;

    // get the fvm and the number of fields
    PetscFV fvm;
    DMGetField(flowData->GetDM(), 0, NULL, (PetscObject *)&fvm) >> errorChecker;
    PetscInt components;
    PetscFVGetNumComponents(fvm, &components) >> errorChecker;

    // Size the error values
    residualNorm2.resize(components);
    residualNormInf.resize(components);

    // Create an vector to hold the exact solution
    Vec exactVec;
    VecDuplicate(flowData->GetSolutionVector(), &exactVec) >> errorChecker;
    DMProjectFunction(flowData->GetDM(), time, exactFuncs, exactCtxs, INSERT_ALL_VALUES, exactVec) >> errorChecker;
    PetscObjectSetName((PetscObject)exactVec, "exact") >> errorChecker;

    // Compute the error
    VecAXPY(exactVec, -1.0, flowData->GetSolutionVector()) >> errorChecker;
    VecSetBlockSize(exactVec, components);
    PetscInt size;
    VecGetSize(exactVec, &size) >> errorChecker;

    // Compute the l2 errors
    VecStrideNormAll(exactVec, NORM_2, &residualNorm2[0]) >> errorChecker;
    // normalize by the number of nodes
    for (std::size_t i = 0; i < residualNorm2.size(); i++) {
        residualNorm2[i] *= PetscSqrtReal(1.0 / (size / components));
    }

    // And the Inf form
    VecStrideNormAll(exactVec, NORM_INFINITY, &residualNormInf[0]) >> errorChecker;
    VecDestroy(&exactVec) >> errorChecker;
}

TEST_P(CompressibleFlowDiffusionTestFixture, ShouldConvergeToExactSolution) {
    StartWithMPI
        PetscErrorCode ierr;

        // initialize petsc and mpi
        PetscInitialize(argc, argv, NULL, "HELP") >> testErrorChecker;

        InputParameters parameters = GetParam().parameters;
        parameters.dim = 2;
        PetscInt blockSize = 2 + parameters.dim;
        PetscInt initialNx = GetParam().initialNx;

        std::vector<PetscReal> hHistory;
        std::vector<std::vector<PetscReal>> l2History(blockSize);
        std::vector<std::vector<PetscReal>> lInfHistory(blockSize);

        // March over each level
        for (PetscInt l = 0; l < GetParam().levels; l++) {
            PetscPrintf(PETSC_COMM_WORLD, "Running Calculation at Level %d\n", l);

            DM dmCreate; /* problem definition */
            TS ts;       /* timestepper */

            // Create a ts
            TSCreate(PETSC_COMM_WORLD, &ts) >> testErrorChecker;
            TSSetProblemType(ts, TS_NONLINEAR) >> testErrorChecker;
            TSSetType(ts, TSEULER) >> testErrorChecker;
            TSSetExactFinalTime(ts, TS_EXACTFINALTIME_MATCHSTEP) >> testErrorChecker;
            TSSetFromOptions(ts) >> testErrorChecker;

            // Create a mesh
            // hard code the problem setup
            PetscReal start[] = {0.0, 0.0};
            PetscReal end[] = {parameters.L, parameters.L};
            PetscInt nx1D = initialNx * PetscPowRealInt(2, l);
            PetscInt nx[] = {nx1D, nx1D};
            DMBoundaryType bcType[] = {DM_BOUNDARY_NONE, DM_BOUNDARY_NONE};
            DMPlexCreateBoxMesh(PETSC_COMM_WORLD, parameters.dim, PETSC_FALSE, nx, start, end, bcType, PETSC_TRUE, &dmCreate) >> testErrorChecker;

            // Setup the flow data
            auto eos = std::make_shared<ablate::eos::PerfectGas>(
                std::make_shared<ablate::parameters::MapParameters>(std::map<std::string, std::string>{{"gamma", std::to_string(parameters.gamma)}, {"Rgas", std::to_string(parameters.Rgas)}}));

            auto flowParameters = std::make_shared<ablate::parameters::MapParameters>(std::map<std::string, std::string>{{"cfl", "0.5"}});

            auto transportModel = std::make_shared<ablate::eos::transport::Constant>(parameters.k);

            auto exactSolution = std::make_shared<mathFunctions::FieldFunction>("euler", mathFunctions::Create(EulerExact, &parameters));

            auto boundaryConditions = std::vector<std::shared_ptr<flow::boundaryConditions::BoundaryCondition>>{
                std::make_shared<flow::boundaryConditions::Ghost>("euler", "wall left/right", std::vector<int>{2, 4}, PhysicsBoundary_Euler, &parameters),
                std::make_shared<flow::boundaryConditions::Ghost>("euler", "top/bottom", std::vector<int>{1, 3}, PhysicsBoundary_Mirror, &parameters),
            };

            auto flowObject = std::make_shared<ablate::flow::CompressibleFlow>("testFlow",
                                                                               std::make_shared<ablate::mesh::DMWrapper>(dmCreate),
                                                                               eos,
                                                                               flowParameters,
                                                                               transportModel,
                                                                               std::make_shared<flow::fluxCalculator::OffFlux>(),
                                                                               nullptr /*options*/,
                                                                               std::vector<std::shared_ptr<mathFunctions::FieldFunction>>{exactSolution} /*initialization*/,
                                                                               boundaryConditions /*boundary conditions*/,
                                                                               std::vector<std::shared_ptr<mathFunctions::FieldFunction>>{exactSolution} /*exactSolution*/);

            flowObject->CompleteProblemSetup(ts);

            // Name the flow field
            PetscObjectSetName(((PetscObject)flowObject->GetSolutionVector()), "Numerical Solution") >> testErrorChecker;

            // Setup the TS
            TSSetFromOptions(ts) >> testErrorChecker;

            // advance to the end time
            TSSolve(ts, flowObject->GetSolutionVector()) >> testErrorChecker;

            // Get the L2 and LInf norms
            std::vector<PetscReal> l2Norm;
            std::vector<PetscReal> lInfNorm;

            // Compute the error
            ComputeErrorNorms(ts, flowObject, l2Norm, lInfNorm, &parameters, testErrorChecker);

            // print the results to help with debug
            auto l2String = PrintVector(l2Norm, "%2.3g");
            PetscPrintf(PETSC_COMM_WORLD, "\tL_2 Error: %s\n", l2String.c_str()) >> testErrorChecker;

            auto lInfString = PrintVector(lInfNorm, "%2.3g");
            PetscPrintf(PETSC_COMM_WORLD, "\tL_2 L_Inf: %s\n", lInfString.c_str()) >> testErrorChecker;

            // Store the residual into history
            hHistory.push_back(PetscLog10Real(parameters.L / nx1D));
            for (auto b = 0; b < blockSize; b++) {
                l2History[b].push_back(PetscLog10Real(l2Norm[b]));
                lInfHistory[b].push_back(PetscLog10Real(lInfNorm[b]));
            }
            TSDestroy(&ts) >> testErrorChecker;
        }

        // Fit each component and output
        for (auto b = 0; b < blockSize; b++) {
            PetscReal l2Slope;
            PetscReal l2Intercept;
            PetscLinearRegression(hHistory.size(), &hHistory[0], &l2History[b][0], &l2Slope, &l2Intercept) >> testErrorChecker;

            PetscReal lInfSlope;
            PetscReal lInfIntercept;
            PetscLinearRegression(hHistory.size(), &hHistory[0], &lInfHistory[b][0], &lInfSlope, &lInfIntercept) >> testErrorChecker;

            PetscPrintf(PETSC_COMM_WORLD, "Convergence[%d]: L2 %2.3g LInf %2.3g \n", b, l2Slope, lInfSlope) >> testErrorChecker;

            if (std::isnan(GetParam().expectedL2Convergence[b])) {
                ASSERT_TRUE(std::isnan(l2Slope)) << "incorrect L2 convergence order for component[" << b << "]";
            } else {
                ASSERT_NEAR(l2Slope, GetParam().expectedL2Convergence[b], 0.2) << "incorrect L2 convergence order for component[" << b << "]";
            }
            if (std::isnan(GetParam().expectedLInfConvergence[b])) {
                ASSERT_TRUE(std::isnan(lInfSlope)) << "incorrect LInf convergence order for component[" << b << "]";
            } else {
                ASSERT_NEAR(lInfSlope, GetParam().expectedLInfConvergence[b], 0.2) << "incorrect LInf convergence order for component[" << b << "]";
            }
        }

        ierr = PetscFinalize();
        exit(ierr);

    EndWithMPI
}

INSTANTIATE_TEST_SUITE_P(
    CompressibleFlow, CompressibleFlowDiffusionTestFixture,
    testing::Values((CompressibleFlowDiffusionTestParameters){.mpiTestParameter = {.testName = "conduction",
                                                                                   .nproc = 1,
                                                                                   .arguments = "-dm_plex_separate_marker -petsclimiter_type none -ts_adapt_type none -automaticTimeStepCalculator off "
                                                                                                "-Tpetscfv_type leastsquares -velpetscfv_type leastsquares -ts_max_steps 600 -ts_dt 0.00000625 "},
                                                              .parameters = {.dim = 2, .L = 0.1, .gamma = 1.4, .Rgas = 1.0, .k = 0.3, .rho = 1.0, .Tinit = 400, .Tboundary = 300},
                                                              .initialNx = 3,
                                                              .levels = 3,
                                                              .expectedL2Convergence = {NAN, 1.5, NAN, NAN},
                                                              .expectedLInfConvergence = {NAN, 1.3, NAN, NAN}},
                    (CompressibleFlowDiffusionTestParameters){.mpiTestParameter = {.testName = "conduction multi mpi",
                                                                                   .nproc = 2,
                                                                                   .arguments = "-dm_plex_separate_marker -petsclimiter_type none -ts_adapt_type none -automaticTimeStepCalculator off "
                                                                                                "-Tpetscfv_type leastsquares -velpetscfv_type leastsquares -ts_max_steps 600 -ts_dt 0.00000625 "},
                                                              .parameters = {.dim = 2, .L = 0.1, .gamma = 1.4, .Rgas = 1.0, .k = 0.3, .rho = 1.0, .Tinit = 400, .Tboundary = 300},
                                                              .initialNx = 9,
                                                              .levels = 2,
                                                              .expectedL2Convergence = {NAN, 2.2, NAN, NAN},
                                                              .expectedLInfConvergence = {NAN, 2.5, NAN, NAN}}),
    [](const testing::TestParamInfo<CompressibleFlowDiffusionTestParameters> &info) { return info.param.mpiTestParameter.getTestName(); });
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct {
    PetscInt dim;
    PetscReal mu;
    std::vector<PetscReal> gradVelL;
    std::vector<PetscReal> gradVelR;
    std::vector<PetscReal> expectedStressTensor;
} StressTensorTestParameters;

class StressTensorTestFixture : public testing::TestWithParam<StressTensorTestParameters> {};

TEST_P(StressTensorTestFixture, ShouldComputeTheCorrectStressTensor) {
    // arrange
    PetscReal computedTau[9];
    const auto &params = GetParam();

    // act
    PetscErrorCode ierr = ablate::flow::processes::EulerDiffusion::CompressibleFlowComputeStressTensor(params.dim, params.mu, &params.gradVelL[0], &params.gradVelR[0], computedTau);

    // assert
    ASSERT_EQ(0, ierr);
    for (auto c = 0; c < params.dim; c++) {
        for (auto d = 0; d < params.dim; d++) {
            auto i = c * params.dim + d;
            ASSERT_NEAR(computedTau[i], params.expectedStressTensor[i], 1E-8) << "The tau component [" + std::to_string(c) + "][" + std::to_string(d) + "] is incorrect";
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    CompressibleFlow, StressTensorTestFixture,
    testing::Values(
        (StressTensorTestParameters){.dim = 1, .mu = .3, .gradVelL = {3.5}, .gradVelR = {3.5}, .expectedStressTensor = {1.4}},
        (StressTensorTestParameters){.dim = 1, .mu = .3, .gradVelL = {4.5}, .gradVelR = {2.5}, .expectedStressTensor = {1.4}},
        (StressTensorTestParameters){.dim = 2, .mu = .3, .gradVelL = {3.5, -2.45, 0, -1}, .gradVelR = {3.5, -2.45, 0, 1}, .expectedStressTensor = {1.4, -0.735, -0.735, -0.7}},
        (StressTensorTestParameters){.dim = 2, .mu = 1.5, .gradVelL = {3.5, -2.45, 0, -6}, .gradVelR = {3.5, -2.45, 0, -8}, .expectedStressTensor = {14, -3.675, -3.675, -17.5}},
        (StressTensorTestParameters){.dim = 2, .mu = 1.5, .gradVelL = {0, -12, 12, 0}, .gradVelR = {0, -12, 12, 0}, .expectedStressTensor = {0, 0, 0, 0}},
        (StressTensorTestParameters){.dim = 2, .mu = 1.5, .gradVelL = {0, -10, 12, 0}, .gradVelR = {0, -20, 12, 0}, .expectedStressTensor = {0, -4.5, -4.5, 0}},
        (StressTensorTestParameters){.dim = 3, .mu = 1.5, .gradVelL = {1, 0, 0, 0, 1, 0, 0, 0, 1}, .gradVelR = {1, 0, 0, 0, 3, 0, 0, 0, 5}, .expectedStressTensor = {-3, 0, 0, 0, 0, 0, 0, 0, 3}},
        (StressTensorTestParameters){
            .dim = 3, .mu = 1.5, .gradVelL = {2, 4, 6, 8, 10, 12, 14, 16, 18}, .gradVelR = {0, 0, 0, 0, 0, 0, 0, 0, 0}, .expectedStressTensor = {-12, 9, 15, 9, 0, 21, 15, 21, 12}},
        (StressTensorTestParameters){
            .dim = 3, .mu = 1.5, .gradVelL = {0, 0, 0, 0, 0, 0, 0, 0, 0}, .gradVelR = {-2, -4, -6, -8, -10, -12, -14, -16, -18}, .expectedStressTensor = {12, -9, -15, -9, 0, -21, -15, -21, -12}},
        (StressTensorTestParameters){
            .dim = 3, .mu = 1.5, .gradVelL = {2, 4, 6, 8, 10, 12, 14, 16, 18}, .gradVelR = {-2, -4, -6, -8, -10, -12, -14, -16, -18}, .expectedStressTensor = {0, 0, 0, 0, 0, 0, 0, 0, 0}},
        (StressTensorTestParameters){.dim = 3, .mu = 0.0, .gradVelL = {1, 2, 3, 4, 5, 6, 7, 8, 9}, .gradVelR = {1, 2, 3, 4, 5, 6, 7, 8, 9}, .expectedStressTensor = {0, 0, 0, 0, 0, 0, 0, 0, 0}},
        (StressTensorTestParameters){.dim = 3, .mu = 0.7, .gradVelL = {0, 0, 0, 0, 0, 0, 0, 0, 0}, .gradVelR = {0, 0, 0, 0, 0, 0, 0, 0, 0}, .expectedStressTensor = {0, 0, 0, 0, 0, 0, 0, 0, 0}}),
    [](const testing::TestParamInfo<StressTensorTestParameters> &info) { return "InputParameters_" + std::to_string(info.index); });