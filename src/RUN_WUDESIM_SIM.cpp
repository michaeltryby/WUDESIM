/*
Project:     WUDESIM ver. 1 BETA
File:        WQSIM.cpp
Author:      Ahmed Abokifa
Date:        10/25/2016
*/


#include <iostream> 
#include <fstream>  
#include <vector>  
#include <map>
#include <algorithm>
#include <sstream>  
#include <string>  
#include <iterator>
#include <numeric>
#include <cmath>

#include "WUDESIM.h"
#include "Classes.h"
#include "WUDESIM_CORE.h"
#include "Utilities.h"
#include "WRITING_FUN.h"


using namespace std;


int RUN_WUDESIM_SIM(Network* net)
{
	/////////////////////////////////////////////INITIALIZE WQ SIMULATION ENGINE/////////////////////////////////////////

	// Find which branches will be simulated
	vector<double> DE_branch_simulation = net->DE_options.simulated_branches;

	// Read stochastic demands flag
	bool stoc_dem_fl = net->DE_options.Stoc_dem_fl;

	// Read EPANET time variables
	int N_steps_EPANET = net->times.N_steps;                                                //Number of hydraulic steps
	double dt_h_EPANET = net->times.Hyd_step_hr + net->times.Hyd_step_min / 60.;            //Hydraulic time step(hr)
	double dt_q_EPANET = net->times.Qual_step_hr * 3600. + net->times.Qual_step_min * 60.;  //User Input Quality time step(sec)

	// Use EPANET time variables by default
	int N_steps_act = N_steps_EPANET;     //Number of hydraulic steps
	double dt_h_act = dt_h_EPANET;        //Hydraulic time step(hr)
	int N_avg_int = 1;

	// unless stochastic demands are turned on
	if (stoc_dem_fl) {
		dt_h_act = net->DE_options.avg_int / 3600.;  // Averaging interval (sec->hr)
		N_avg_int = dt_h_EPANET / dt_h_act;           // Number of averaging intervals per EPANET hydraulic step
		N_steps_act = N_steps_EPANET * N_avg_int;       // Number of simulation hydraulic steps	
	}

	// Set reactions variables
	double Kb = net->reactions.Bulk_coeff;      //first order bulk decay coefficient(/sec)
	double Kw = net->reactions.Wall_coeff;      //first order wall decay coefficeint(m/sec) or (1/m2/sec)
	double n_b = net->reactions.Bulk_order;	    //Bulk reaction order
	double n_w = net->reactions.Wall_order;		//Wall reaction order
	double C_L = net->reactions.Lim_pot;		//Limiting potential

	// Set transport variables
	double D_diff    = net->options.Rel_Diffusivity * 1.2E-9;      //molecular diffusion coefficient(m2 / sec)
	double viscosity = net->options.Rel_Viscosity * 1E-6;          //water kinematic viscosity(m2 / sec) = 1cSt
	double Sc = viscosity / D_diff;                                //Schmidt Number

	// Set Correction Factors
	CALC_CORR_FACT(net);

	/////////////////////////////////////////////Loop for Dead-End Branches/////////////////////////////////////////

	for (int bb = 0; bb < DE_branch_simulation.size(); bb++) {

		int branch = DE_branch_simulation[bb];

		cout << "	o	Simulating Dead-End Branch no." << branch + 1 << endl;

		// Get number of branches
		int N_pipes = net->DE_branches[branch].branch_size;

		//Reset WQ to the User Input Quality time step(sec) in case it was changed in the previous branch
		double dt_q = dt_q_EPANET;

		
		//////////////////////////////////Initialize Vectors///////////////////////////////////////////////

		// Flow rate, veclity, and reynolds number
		vector<vector<double>> pipe_flow(N_pipes, vector<double>(N_steps_act, 0.));
		vector<vector<double>> u(N_pipes, vector<double>(N_steps_act, 0.));
		vector<vector<double>> u_corr(N_pipes, vector<double>(N_steps_act, 0.));
		vector<vector<double>> Re(N_pipes, vector<double>(N_steps_act, 0.));

		// Geometry
		vector<double> dp(N_pipes);
		vector<double> Lt(N_pipes);
		vector<double> r0(N_pipes);
		vector<double> Ap(N_pipes);

		// Correction factors
		vector<double> N_seg_corr(N_pipes);
		vector<double> Flow_Corr(N_pipes);
		vector<double> Disp_Corr(N_pipes);
		vector<double> Kw_Corr(N_pipes);

		// Space discretization
		vector<vector<double>> N(N_pipes, vector<double>(N_steps_act, 0.));
		vector<vector<double>> dx(N_pipes, vector<double>(N_steps_act, 0.));

		// Dispersion coefficient and Peclet number
		vector<vector<double>> E_disp(N_pipes, vector<double>(N_steps_act, 0.));
		net->DE_branches[branch].Peclet.resize(N_pipes, vector<double>(N_steps_act, 0.));

		// Terminal WUDESIM concentration
		net->DE_branches[branch].terminal_C_WUDESIM.resize(N_pipes, vector<double>(N_steps_act, 0.));

		//Calculate decay coefficients
		vector<vector<double>> Sh(N_pipes, vector<double>(N_steps_act, 0.));
		vector<vector<double>> Kf(N_pipes, vector<double>(N_steps_act, 0.));
		vector<vector<double>> K(N_pipes, vector<double>(N_steps_act, 0.));

		///////////////////////////////////////////////// Read Input Data /////////////////////////////////////////////////////

		for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

			//Read pipe flow
			if (!stoc_dem_fl) {
				pipe_flow[DeadEnd] = net->DE_branches[branch].pipe_flow_EPANET[DeadEnd];
			}
			else {
				pipe_flow[DeadEnd] = net->DE_branches[branch].pipe_flow_WUDESIM[DeadEnd];
			}

			// Pipe data
			dp[DeadEnd] = net->DE_branches[branch].diameter[DeadEnd]; //pipe diameter (m)
			Lt[DeadEnd] = net->DE_branches[branch].length[DeadEnd];   //pipe length (m)
			r0[DeadEnd] = dp[DeadEnd] / 2.;                                    //pipe radius(m)
			Ap[DeadEnd] = 3.14159 * pow(r0[DeadEnd], 2.);                      //pipe x - sec area(m2)

			// Read correction factors
			N_seg_corr[DeadEnd] = net->DE_branches[branch].N_segment[DeadEnd];
			Flow_Corr[DeadEnd] = net->DE_branches[branch].Correction_factors[DeadEnd][0];
			Disp_Corr[DeadEnd] = net->DE_branches[branch].Correction_factors[DeadEnd][1];
			Kw_Corr[DeadEnd] = net->DE_branches[branch].Correction_factors[DeadEnd][2];

			// Flow velocity
			for (int i = 0; i < N_steps_act; ++i) {

				u[DeadEnd][i] = pipe_flow[DeadEnd][i] / Ap[DeadEnd];   //Actual Flow velocity(m / sec)
				u_corr[DeadEnd][i] = u[DeadEnd][i] * Flow_Corr[DeadEnd];    //Corrected Flow velocity(m / sec)

				if (Lt[DeadEnd] / u_corr[DeadEnd][i] > 3600. * 24. * 7.) { u_corr[DeadEnd][i] = 0; } //Consider stagnant if corrected residence time is more than 24 hrs

				Re[DeadEnd][i] = u[DeadEnd][i] * dp[DeadEnd] / viscosity;         //Calculate Reynolds Number

			}

			//Read terminal concentration from EPANET for the first time step
			net->DE_branches[branch].terminal_C_WUDESIM[DeadEnd][0] = net->DE_branches[branch].terminal_C_EPANET[DeadEnd][0];

		}

		//////////////////////////////////////////SPACE TIME DISCRETIZATION/////////////////////////////////////////////////////

		//Check whether the user selected quality time step is sufficient for max flow event in shortest pipe in the branch

		for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

			double u_max = abs(*max_element(u_corr[DeadEnd].begin(), u_corr[DeadEnd].end()));     //maximum flow velocity(m / sec)
			double dx_max = u_max * dt_q;              //maximum delta x(m)
			double N_min = Lt[DeadEnd] / dx_max + 1;   //min number of discretization points

			if (N_min < 5) {						   //Should have at least ten discretization points
				N_min = 5;
				dx_max = Lt[DeadEnd] / (N_min - 1);
				dt_q = dx_max / u_max;
			}
		}

		//Time discretization
		double Tot_time = dt_h_act * N_steps_act * 3600;      //Total time(sec)
		double Nqsteps = ceil(dt_h_act * 3600 / dt_q);        //No.of quality steps
		dt_q = dt_h_act * 3600 / Nqsteps;                     //Actual quality time step(sec)

		if (dt_q != dt_q_EPANET) {
			cout << "			Water Quality Step Reduced to " << dt_q << "s" << endl;
		}

		// Space discretaztion
		for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

			for (int i = 0; i < N_steps_act; ++i) {

				N[DeadEnd][i] = floor(Lt[DeadEnd] / (u_corr[DeadEnd][i] * dt_q)) + 1;

				if (isinf(N[DeadEnd][i])) { N[DeadEnd][i] = 100; };

				dx[DeadEnd][i] = Lt[DeadEnd] / (N[DeadEnd][i] - 1);

			}
		}

		////////////////////////////////////////////GET the BOUNDARY CONDITION///////////////////////////////////////////////
				
		// Get the boundary concentration of the dead-end branch
		vector<double> DEboundary(N_steps_act, 0);
		if (stoc_dem_fl) {

			int kk = 0;
			for (int epanet_step = 0; epanet_step < N_steps_EPANET; ++epanet_step) {
				for (int interv = 0; interv < N_avg_int; ++interv) {

					DEboundary[kk] = net->DE_branches[branch].boundary_C_EPANET[N_pipes - 1][epanet_step];
					++kk;

				}
			}
		}
		else {
			DEboundary = net->DE_branches[branch].boundary_C_EPANET[N_pipes - 1];
		}

		// Boundary condition interpolation
		vector<double> C_bound_in(N_steps_act + 1);
		vector<double> Times_bound_in(N_steps_act + 1);

		for (int i = 0; i < N_steps_act; ++i) {
			C_bound_in[i]     = DEboundary[i];
			Times_bound_in[i] = i * dt_h_act;
		}
		C_bound_in[N_steps_act] = C_bound_in[N_steps_act - 1];    //Add another concentration to interpolate the last time step
		Times_bound_in[N_steps_act] = N_steps_act * dt_h_act;

		vector<double> Times_bound_act(Nqsteps* N_steps_act);
		vector<double> C_bound_act(Nqsteps* N_steps_act);
		for (int i = 0; i < Times_bound_act.size(); ++i) { Times_bound_act[i] = i * dt_q / 3600; }

		C_bound_act = interpolation(Times_bound_in, C_bound_in, Times_bound_act);
		
		////////////////////////////////////////////CALCULATE DISPERSION COEFFICIENTS//////////////////////////////////////

		if (net->DE_options.Dispersion_fl != 0) {

			for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

				for (int Hstep = 0; Hstep < N_steps_act; ++Hstep) {

					// Taylor dispersion for laminar regime
					double E_taylor_lam = (pow(r0[DeadEnd] * u[DeadEnd][Hstep], 2) / (48. * D_diff));

					// Taylor dispersion for turbulent regime
					double aaa = 0.15 * pow(10, -3) / (3.7 * dp[DeadEnd]);
					double bbb = 5.74 / (pow(Re[DeadEnd][Hstep], 0.9));
					double f = 0.25 / pow(log(aaa + bbb), 2); //Darcy friction factor (swamee-jain)

					double E_taylor_tur = 10.1 * r0[DeadEnd] * abs(u[DeadEnd][Hstep]) * sqrt(f / 8);

					// Sattar GEP-2 dispersion for transitional and turbulent regime
					double E_sattar = 4110 * (2 * u[DeadEnd][Hstep] + 0.062) * dp[DeadEnd] / (Re[DeadEnd][Hstep]);


					//Calculate the dispersion coefficient
					if (Re[DeadEnd][Hstep] < 2300) {  //laminar flow

						if (net->DE_options.Dispersion_fl == 1) { // use Taylor dispersion for laminar flow

							E_disp[DeadEnd][Hstep] = E_taylor_lam * Disp_Corr[DeadEnd] + D_diff;

						}

						else if (net->DE_options.Dispersion_fl == 2) {   //use Lee 2004 average dispersion rates

							double T0 = pow(r0[DeadEnd], 2.) / (16 * D_diff); //Eularian Time-scale
							double T_res = Lt[DeadEnd] / u[DeadEnd][Hstep];   // Residence Time

							E_disp[DeadEnd][Hstep] = E_taylor_lam * (1 - (T0 / T_res) * (1 - exp(-T_res / T0))) + D_diff;

						}
					}

					else if (Re[DeadEnd][Hstep] < 4000) {  //Transitional flow --> interpolate to get dispersion	coefficent						

						E_disp[DeadEnd][Hstep] = E_taylor_lam + (E_sattar - E_taylor_lam) * ((Re[DeadEnd][Hstep] - 2300) / 1700) + D_diff;

					}

					else {

						E_disp[DeadEnd][Hstep] = E_sattar + D_diff;

					}

					// Calculate Peclet number
					net->DE_branches[branch].Peclet[DeadEnd][Hstep] = u[DeadEnd][Hstep] * Lt[DeadEnd] / E_disp[DeadEnd][Hstep];

				} // loop for hydraulic steps

			} // loop for Deadend branches

		}

		////////////////////////////////////////////CALCULATE MASS TRANSFER COEFFICIENTS//////////////////////////////////////

		for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

			for (int i = 0; i < N_steps_act; ++i) {

				double Reyn = Re[0][i] * Flow_Corr[DeadEnd];

				//Sherwood Numbr
				if (Reyn >= 2300) {

					// For turbulent flow --> corrected Reynolds number is used to give reduced mass transfer
					Sh[DeadEnd][i] = 0.023 * pow(Reyn , 0.83) * pow(Sc, 0.333);

				}
				else {
					
					// For laminar flow --> original Reynolds number is used because the wall demand will later be corrected
					Sh[DeadEnd][i] = 3.65 + (0.0668 * (dp[DeadEnd] * Reyn * Sc / Lt[DeadEnd])) / (1 + 0.04 * pow((dp[DeadEnd] * Reyn * Sc / Lt[DeadEnd]), (2 / 3)));

				}

				//Mass transfer coefficient
				Kf[DeadEnd][i] = Sh[DeadEnd][i] * D_diff / dp[DeadEnd];
			}
		}

		///////////////////////////////////////////////////////////////////////////////////////////////////////////////
		/////////////////////////////////////////////WATER QUALITY SIMULATIONS/////////////////////////////////////////
		///////////////////////////////////////////////////////////////////////////////////////////////////////////////

		// Initialize pipe concentration vectors
		vector<vector<double>> C(N_pipes);
		vector<vector<double>> C_adv(N_pipes);
		vector<vector<double>> C_init(N_pipes);
		
		// Initialize pipe discretization vectors
		vector<vector<double>> x(N_pipes);
		vector<vector<double>> eps(N_pipes);

		// Initialize nodal concentration vectors
		vector<vector<double>> Cn(N_pipes+1, vector<double> (Nqsteps*N_steps_act,0));
		vector<vector<double>> Cn_adv(N_pipes + 1, vector<double>(Nqsteps * N_steps_act, 0));

		// Set boundary concentration for the branch
		Cn[N_pipes]     = C_bound_act;
		Cn_adv[N_pipes] = C_bound_act;

		// Initialize dispersion matrices
		vector<vector<double>> H_save(N_pipes);
		vector<vector<double>> GR_save(N_pipes);
		vector<vector<double>> GF_save(N_pipes);

		/////////////////////////////////////////////Loop for Hydraylic Steps/////////////////////////////////////////
		
		// quality steps counter
		int t = 0;

		for (int Hstep = 0; Hstep < N_steps_act - 1; ++Hstep) {
			
			/////////////////////////////////////////////Setup Initial Condition/////////////////////////////////////////
			
			for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {
				
				// Get number of disc. points
				int n_pts = N[DeadEnd][Hstep];

				// Discretization
				x[DeadEnd].resize(n_pts, 0);
				eps[DeadEnd].resize(n_pts, 0);
				for (int i = 0; i < n_pts; ++i) {
					x[DeadEnd][i]   = i * dx[DeadEnd][Hstep];                 //Discretized space co - ordinate
					eps[DeadEnd][i] = x[DeadEnd][i] - u_corr[DeadEnd][Hstep] * dt_q;   //Characteristic line footprint
				}

				// Set initial concentration							
				if (t == 0) {
					// Initialize concentration in the pipe (equal to downstream node concentration in EPANET)
					double init_conc = net->DE_branches[branch].terminal_C_EPANET[DeadEnd][0];
					C_init[DeadEnd].resize(n_pts, init_conc);
				}

				// Initialize pipe concentrations
				C[DeadEnd].resize(n_pts, 0);
				C[DeadEnd] = C_init[DeadEnd];
			}

			/////////////////////////////////////////////Loop for Water Quality Steps/////////////////////////////////////////
			
			for (int qstep = 0; qstep < Nqsteps; ++qstep) {

				
				/////////////////////////////////////////////ADVECTION STEP/////////////////////////////////////////
				
				for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

					// Get number of disc. points
					int n_pts = N[DeadEnd][Hstep];

					// Initialize advection concentration
					C_adv[DeadEnd].resize(n_pts, 0);

					// Get boundary concentration
					C_adv[DeadEnd][0] = Cn[DeadEnd + 1][t];

					// Check if advection is needed
					if (u[DeadEnd][Hstep] != 0.) {

						for (int i = 1; i < n_pts; ++i) {

							// MOC advection step
							C_adv[DeadEnd][i] = (C[DeadEnd][i - 1] * (x[DeadEnd][i] - eps[DeadEnd][i]) + C[DeadEnd][i] * (eps[DeadEnd][i] - x[DeadEnd][i - 1])) / dx[DeadEnd][Hstep];
												
						}						
					}
					else {
						C_adv[DeadEnd] = C[DeadEnd];
					}


				}

				/////////////////////////////////////////////REACTION STEP/////////////////////////////////////////
				for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

					// Get number of disc. points
					int n_pts = N[DeadEnd][Hstep];

					for (int i = 1; i < n_pts; ++i) {

						//Bulk Reaction step
						if (C_L == 0) {

							if (n_b == 1) {
								//First order bulk reaction
								C_adv[DeadEnd][i] *= exp(Kb * dt_q);
							}
							else { //nth order bulk reaction
								C_adv[DeadEnd][i] *= (1 + (n_b - 1) * Kb * pow(C_adv[DeadEnd][i], n_b - 1) * dt_q);
							}
						}
						else if (n_b > 0) { // dC/dt=Kb*(C_L-C)*C^(n_b-1)
							//Solve via RK4
							double y1 = C_adv[DeadEnd][i];
							double k1 = Kb * (C_L - y1) * pow(y1, n_b - 1);
							double y2 = y1 + dt_q * k1 / 2;
							double k2 = Kb * (C_L - y2) * pow(y2, n_b - 1);
							double y3 = y1 + dt_q * k2 / 2;
							double k3 = Kb * (C_L - y3) * pow(y3, n_b - 1);
							double y4 = y1 + dt_q * k3;
							double k4 = Kb * (C_L - y4) * pow(y4, n_b - 1);

							C_adv[DeadEnd][i] += (dt_q / 6) * (k1 + 2 * k2 + 2 * k3 + k4);
						}
						else if (n_b < 0) {
							if (Kb < 0) {      // Michaelis-Menton Decay Kinetics: dC/dt=Kb*C/(C_L-C)
								double y1 = C_adv[DeadEnd][i];
								double k1 = Kb * y1 / (C_L - y1);

								double y2 = y1 + dt_q * k1 / 2;
								double k2 = Kb * y2 / (C_L - y2);

								double y3 = y1 + dt_q * k2 / 2;
								double k3 = Kb * y3 / (C_L - y3);

								double y4 = y1 + dt_q * k3;
								double k4 = Kb * y4 / (C_L - y4);

								C_adv[DeadEnd][i] += (dt_q / 6) * (k1 + 2 * k2 + 2 * k3 + k4);

							}
							else if (Kb > 0) { //Michaelis-Menton growth Kinetics dC/dt=Kb*C/(C_L+C)
								double y1 = C_adv[DeadEnd][i];
								double k1 = Kb * y1 / (C_L + y1);

								double y2 = y1 + dt_q * k1 / 2;
								double k2 = Kb * y2 / (C_L + y2);

								double y3 = y1 + dt_q * k2 / 2;
								double k3 = Kb * y3 / (C_L + y3);

								double y4 = y1 + dt_q * k3;
								double k4 = Kb * y4 / (C_L + y4);

								C_adv[DeadEnd][i] += (dt_q / 6) * (k1 + 2 * k2 + 2 * k3 + k4);
							}
						}

						//Wall Reaction 
						double Rw;
						if (n_w == 1) {        //First order wall reaction

							if (Re[DeadEnd][Hstep] < 2300) {
								// include correction factor only for laminar flow
								Rw = (abs(Kw) * Kf[DeadEnd][Hstep]) / ((abs(Kw) + Kf[DeadEnd][Hstep]) * dp[DeadEnd] / 4.) * Kw_Corr[DeadEnd];
							}
							
							else
							{
								Rw = (abs(Kw) * Kf[DeadEnd][Hstep]) / ((abs(Kw) + Kf[DeadEnd][Hstep]) * dp[DeadEnd] / 4.);
							}

							if (Kw < 0) {
								C_adv[DeadEnd][i] *= exp(-Rw * dt_q);
							}

							else if (Kw > 0) {
								C_adv[DeadEnd][i] *= exp(Rw * dt_q);
							}
						}
						else if (n_w == 0) {            // Zeroth order wall reaction
							Rw = (4 * Kw / dp[DeadEnd]) / 1000;  // Kw/rh (mg/m3/sec --> mg/L/sec)
							C_adv[DeadEnd][i] += Rw * dt_q;
						}
					}
				}
				
				/////////////////////////////////////////////Update nodal concentrations/////////////////////////////////////////

				// update advection concentrations of the connecting nodes
				for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {
					
					int n_pts = N[DeadEnd][Hstep];

					Cn_adv[DeadEnd][t + 1] = C_adv[DeadEnd][n_pts - 1];
				}

				/////////////////////////////////////////////DISPERSION STEP/////////////////////////////////////////
				
				// Check if dispersion is turned on
				if (net->DE_options.Dispersion_fl != 0)
				{
					
					/////////////////////////////////////////////Calculate Dispersion Matrices for all pipes/////////////////////////////////////////
					for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

						// Get number of disc. points
						int n_pts = N[DeadEnd][Hstep];

						// Initialize matrices
						vector<double> H(n_pts, 0);
						vector<double> GR(n_pts, 0);
						vector<double> GF(n_pts, 0);

						//Calculate lambda
						double  L = E_disp[DeadEnd][Hstep] * dt_q / pow(dx[DeadEnd][Hstep], 2);

						//Create the tridiagonal matrix (A)
						int n = n_pts - 2;
						
						vector<double> a1(n);
						vector<double> a2(n);
						vector<double> a3(n);
						
						fill(a1.begin() + 1, a1.end(), -L);
						fill(a2.begin(), a2.end(), 1 + 2 * L);
						fill(a3.begin(), a3.end() - 1, -L);

						// Solve for H
						vector<double> H_str(n);
						for (int i = 0; i < n; ++i) { H_str[i] = C_adv[DeadEnd][i + 1]; }

						solveThomas(a1, a2, a3, H_str, n);
						for (int i = 1; i <= n; ++i) { H[i] = H_str[i - 1];}


						// Solve for GR
						vector<double> GR_str(n);
						GR_str.front() = L;

						solveThomas(a1, a2, a3, GR_str, n);
						GR.front() = 1.;
						for (int i = 1; i <= n; ++i) { GR[i] = GR_str[i - 1]; }

						// Solve for GF
						vector<double> GF_str(n);
						GF_str.back() = L;

						solveThomas(a1, a2, a3, GF_str, n);
						GF.back() = 1.;
						for (int i = 1; i <= n; ++i) { GF[i] = GF_str[i - 1]; }

						// Save the matrices
						H_save[DeadEnd] = H;
						GF_save[DeadEnd] = GF;
						GR_save[DeadEnd] = GR;
					}


					/////////////////////////////////////////////Calculate nodal concentrations/////////////////////////////////////////
					{
						vector<double> AA(N_pipes + 1, 0);
						vector<double> BB(N_pipes + 1, 0);
						vector<double> CC(N_pipes + 1, 0);
						vector<double> DD(N_pipes + 1, 0);

						// Get boundary nodes from advection
						BB[0] = 1;
						BB[N_pipes] = 1;

						DD[0] = Cn_adv[0][t + 1];						
						DD[N_pipes] = Cn_adv[N_pipes][t + 1];

						for (int i = 1; i <= (N_pipes - 1); i++) {					
							
							// Get number of disc. points
							int n_pts = N[i][Hstep];

							double m1 = E_disp[i-1][Hstep] / dx[i-1][Hstep];
							double m2 = E_disp[i][Hstep] / dx[i][Hstep];

							double m = (dx[i][Hstep] + dx[i - 1][Hstep]) / (2 * dt_q);

							double H1 = H_save[i-1][1];
							double GR1 = GR_save[i-1][1];
							double GF1 = GF_save[i-1][1];

							double H2 = H_save[i][n_pts-2];
							double GR2 = GR_save[i][n_pts - 2];
							double GF2 = GF_save[i][n_pts - 2];


							AA[i] = -m1 * GF1;
							BB[i] = m + m1 * (1 - GR1) + m2 * (1 - GF2);
							CC[i] = -m2 * GR2;
							DD[i] = m * Cn_adv[i][t + 1] + m1 * H1 + m2 * H2;

						}

						// Solve thomas algorithm
						solveThomas(AA, BB, CC, DD, DD.size());

						for (int i = 0; i < N_pipes;i++) {
							Cn[i][t + 1] = DD[i]; // Cn_adv[DeadEnd][t + 1];// 
						}
					}

					/////////////////////////////////////////////Calculate Pipe concentrations/////////////////////////////////////////

					for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {


						// Get number of disc. points
						int n_pts = N[DeadEnd][Hstep];

						for (int i = 0; i < n_pts; ++i) {
							C[DeadEnd][i] = H_save[DeadEnd][i] + GR_save[DeadEnd][i] * Cn[DeadEnd + 1][t+1] + GF_save[DeadEnd][i] * Cn[DeadEnd][t+1];
						}

					}
				}			

				else {
					for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {
						C[DeadEnd] = C_adv[DeadEnd];
						Cn[DeadEnd][t+1] = Cn_adv[DeadEnd][t+1];
					}

				}
				
				++t;

			} // Loop for quality time steps						
			
			
			// Store terminal concentration && Interpolate to initial profile for next hydraulic step

			for (int DeadEnd = (N_pipes - 1); DeadEnd >= 0; DeadEnd--) {

				int n_pts = N[DeadEnd][Hstep];

				// Store terminal concentration
				int T = Hstep + 1;
				net->DE_branches[branch].terminal_C_WUDESIM[DeadEnd][T] = Cn[DeadEnd][t];

				if (abs(net->DE_branches[branch].terminal_C_WUDESIM[DeadEnd][T]) < 0.0001) {
					net->DE_branches[branch].terminal_C_WUDESIM[DeadEnd][T] = 0;
				}

				// Now interpolate the concentration profile to fit the new hydraulic step
				if (Hstep < N_steps_act - 1) {

					C_init[DeadEnd].resize(N[DeadEnd][Hstep + 1],0);

					vector<double> x_next(N[DeadEnd][Hstep + 1],0);

					for (int j = 0; j < N[DeadEnd][Hstep + 1]; ++j) {
						x_next[j] = j * dx[DeadEnd][Hstep + 1];
					}
					C_init[DeadEnd] = interpolation(x[DeadEnd], C[DeadEnd], x_next);
				}
			}

		} // Loop for hydraulic time steps

	} // Loop for dead-end branches

	return 0;
}

