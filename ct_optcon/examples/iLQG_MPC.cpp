
#include <ct/optcon/optcon.h>
#include "exampleDir.h"

using namespace ct::core;
using namespace ct::optcon;

/*!
 * This example shows how to use the MPC class. Here, we apply iLQG-MPC to a simple second order system, a damped oscillator.
 *
 * \example iLQG_MPC.cpp
 */
int main(int argc, char **argv)
{
	// get the state and control input dimension of the oscillator
	const size_t state_dim = ct::core::SecondOrderSystem::STATE_DIM;
	const size_t control_dim = ct::core::SecondOrderSystem::CONTROL_DIM;


	/* STEP 1: set up the Optimal Control Problem
	 * First of all, we create instances of the system dynamics, the linearized system and the cost function.
	 */

	// create a instance of the oscillator dynamics for the optimal control problem
	double w_n(0.1);
	std::shared_ptr<ct::core::ControlledSystem<state_dim, control_dim> > oscillatorDynamics(
			new ct::core::SecondOrderSystem(w_n, 5.0));

	// create a System Linearizer. For simplicity, we use the Num-diff Linearizer.
	std::shared_ptr<ct::core::SystemLinearizer<state_dim, control_dim>> adLinearizer(
			new ct::core::SystemLinearizer<state_dim, control_dim> (oscillatorDynamics));

	// load the cost weighting matrices from file and store them in terms. Note that we define both intermediate and terminal cost
	std::shared_ptr<ct::optcon::TermQuadratic<state_dim, control_dim>> intermediateCost (new ct::optcon::TermQuadratic<state_dim, control_dim>());
	std::shared_ptr<ct::optcon::TermQuadratic<state_dim, control_dim>> finalCost (new ct::optcon::TermQuadratic<state_dim, control_dim>());
	intermediateCost->loadConfigFile(ct::optcon::exampleDir+"/mpcCost.info", "intermediateCost", true);
	finalCost->loadConfigFile(ct::optcon::exampleDir+"/mpcCost.info", "finalCost", true);

	// create a cost function and add the terms to it.
	std::shared_ptr<CostFunctionQuadratic<state_dim, control_dim>> costFunction (new CostFunctionAnalytical<state_dim, control_dim>());
	costFunction->addIntermediateTerm(intermediateCost);
	costFunction->addFinalTerm(finalCost);

	// in this example, we choose a random initial state x0
	StateVector<state_dim> x0;
	x0.setRandom();

	// and a final time horizon in [sec]
	ct::core::Time timeHorizon = 3.0;

	// set up and initialize optimal control problem
	OptConProblem<state_dim, control_dim> optConProblem (oscillatorDynamics, costFunction, adLinearizer);
	optConProblem.setInitialState(x0);
	optConProblem.setTimeHorizon(timeHorizon);



	/* STEP 2: solve the optimal control problem using iLQG
	 * iLQG-MPC works best if it's supplied with a good initial guess. If possible, and given that your
	 * control system is in a steady state at start, we recommend to solve the full Optimal Control problem
	 * first, start executing the policy and at the same time re-using the optimal solution as initial guess for MPC.
	 */

	// initial iLQG settings (default settings except for dt)
	iLQGSettings ilqg_settings;
	ilqg_settings.dt = 0.001;
	ilqg_settings.dt_sim = 0.001;

	// calculate the number of time steps
	size_t K = std::round(timeHorizon / ilqg_settings.dt);

	// provide trivial initial controller to iLQG
	FeedbackArray<state_dim, control_dim> u0_fb(K, FeedbackMatrix<state_dim, control_dim>::Zero());
	ControlVectorArray<control_dim> u0_ff(K, ControlVector<control_dim>::Zero());
	ct::core::StateFeedbackController<state_dim, control_dim> initController (u0_ff, u0_fb, ilqg_settings.dt);

	// create an iLQG instance
	iLQG<state_dim, control_dim> iLQG_init (optConProblem, ilqg_settings);

	// configure it and set the initial guess
	iLQG_init.configure(ilqg_settings);
	iLQG_init.setInitialGuess(initController);

	// and finally solve the optimal control problem
	iLQG_init.solve();

	// now obtain the optimal controller, which we will use to initialize MPC later on
	ct::core::StateFeedbackController<state_dim, control_dim> perfectInitController = iLQG_init.getSolution();
	ct::core::StateTrajectory<state_dim> perfectStateTrajectory = iLQG_init.getStateTrajectory();



	/* STEP 3: set up MPC
	 * Next, we set up an MPC instance for the iLQG solver and configure it.
	 */

	// settings for the ilqg instance used in MPC
	iLQGSettings ilqg_settings_mpc;
	ilqg_settings_mpc.dt = 0.001;
	ilqg_settings_mpc.dt_sim = 0.001;

	// in MPC-mode, it usually makes sense to limit the overall number of iLQG iterations in order to avoid too unpredictable time variations
	ilqg_settings_mpc.max_iterations = 5;


	// fill in mpc specific settings. For a more detailed description of those, visit ct/optcon/mpc/MpcSettings.h
	ct::optcon::mpc_settings settings;
	settings.stateForwardIntegration_ = true;
	settings.postTruncation_ = true;
	settings.measureDelay_ = true;
	settings.delayMeasurementMultiplier_ = 1.0;
	settings.mpc_mode = ct::optcon::MPC_MODE::FIXED_FINAL_TIME;
	settings.coldStart_ = false;
	settings.additionalDelayUs_ = 0;


	// Create the iLQG-MPC object
	MPC<iLQG<state_dim, control_dim>> ilqg_mpc (optConProblem, ilqg_settings_mpc, settings);

	// initialize it using the previously computed initial controller
	ilqg_mpc.setInitialGuess(perfectInitController);



	/* STEP 4: running MPC
	 * Here, we run the MPC loop. Note that the general underlying idea is that you receive a state-estimate
	 * together with a time-stamp from your robot or system. MPC needs to receive both that time information and
	 * the state from your control system. Here, "simulate" the time measurement using std::chrono and wrap
	 * everything into a for-loop.
	 * The basic idea of operation is that after receiving time and state information, one executes the run() method of MPC.
	 */
	auto start_time = std::chrono::high_resolution_clock::now();


	// outputs
	ct::core::StateTrajectory<state_dim> stateTraj;

	// limit the maximum number of runs in this example
	size_t maxNumRuns = 2000;

	std::cout << "Starting to run MPC" << std::endl;

	for(size_t i = 0; i<maxNumRuns; i++)
	{
		// let's for simplicity, assume that the "measured" state is the first state from the optimal trajectory plus some noise
		if(i>0)
			x0 = stateTraj.front() + 0.1*StateVector<state_dim>::Random();

		// time which has passed since start of MPC
		auto current_time = std::chrono::high_resolution_clock::now();
		ct::core::Time t = 1e-6 * std::chrono::duration_cast<std::chrono::microseconds>(current_time - start_time).count();

		// new optimal policy
		ct::core::StateFeedbackController<state_dim, control_dim> newPolicy;

		// timestamp of the new optimal policy
		ct::core::Time ts_newPolicy;

		// !!! run one MPC cycle !!! (get new policy by reference)
		bool success = ilqg_mpc.run(x0, t, newPolicy, ts_newPolicy);

		// retrieve the currently optimal state trajectory
		stateTraj = ilqg_mpc.getStateTrajectory();

		// we break the loop in case the time horizon is reached or solve() failed
		if(ilqg_mpc.timeHorizonReached() | !success)
			break;
	}


	// the summary contains some statistical data about time delays, etc.
	ilqg_mpc.printMpcSummary();

}
