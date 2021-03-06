#include "itensor/all.h"
#include <iostream>
#include "TStateObserver.h"
//#include "S2.h"

using namespace std;
using namespace itensor;

using TensorT = ITensor;
using MPOT = MPOt<TensorT>;
using MPST = MPSt<TensorT>;

double** get_rdm1up(MPST psi, int N);
double** get_rdm1dn(MPST psi, int N);
double *get_rdm2diag(MPST psi, int N);
double*** get_rdm1s(MPST psi, int N);
MPST rk4_fit_1timestep(MPST psi, double tau, MPOT H, Args args);
MPST rk4_exact_1timestep(MPST psi, double tau, MPOT H, Args args);


int 
main(int argc, char* argv[])
    {
    printfln("TensorT == %s",(std::is_same<TensorT,ITensor>::value ? "ITensor" : "IQTensor"));

    //Get parameter file
    if(argc != 2)
        {
        printfln("Usage: %s inputfile.",argv[0]);
        return 0;
        }
    auto input = InputGroup(argv[1],"input");

    auto N = input.getInt("N",10);
    auto U = input.getReal("U", 1.0);
    auto outdir = input.getString("outdir");
    auto mu = input.getReal("mu", 1.0);
    auto t1 = input.getReal("t", 1.0);
    auto delta = input.getReal("delta", 1.0);
    auto periodic = input.getYesNo("periodic",false);

    auto beta = input.getReal("beta",1);
    auto tau = input.getReal("tau",0.005);

    auto maxm = input.getInt("maxm",1000);
    auto cutoff = input.getReal("cutoff",1E-11);

    auto realstep = input.getYesNo("realstep",false);
    auto verbose = input.getYesNo("verbose",false);
    auto rungekutta = input.getYesNo("rungekutta", true);
    auto fitmpo = input.getYesNo("fitmpo", true);

    //auto N = Nx*Ny;

    Args args;
    args.add("N",N);
    args.add("Maxm",maxm);
    args.add("Cutoff",cutoff);
    args.add("Verbose",verbose);
    args.add("Normalize",false);

    auto sites = Hubbard(2*N, {"ConserveNf",false,"ConserveSz", true});
    //auto sites = Hubbard(2*N, {"ConserveNf",true,"ConserveSz", true});
    //auto sites = Hubbard(2*N);

    
    ////////////////////////////////////////////////////////
    //               Construct Hamitltonian               //
    ////////////////////////////////////////////////////////
    auto ampo = AutoMPO(sites);

    //////////////////
    // Coulomb term //
    //////////////////
    for(int i=1; i<=N;++i ) 
    {
        int s1 = 2*i-1;
        ampo += U, "Nupdn", s1;
    }
    ///////////////////////////
    // nearest neighbor term //
    ///////////////////////////

    int endpnt; //PBC or OBC
    if(periodic) endpnt = N;
    else endpnt = N-1;
    
    for(int i=1; i<=endpnt;++i )
    {
        int s1 = 2*i-1;
        int s2 = 2*(i+1)-1;
        if(i==N) s2 = 1;
        ampo += -1*t1,"Cdagup",s1,"Cup",s2;
        ampo += -1*t1,"Cdagup",s2,"Cup",s1;
        ampo += -1*t1,"Cdagdn",s1,"Cdn",s2;
        ampo += -1*t1,"Cdagdn",s2,"Cdn",s1;
    }
    /////////////////////////////
    // chemical potential term //
    /////////////////////////////

    for(int i=1; i<=N; ++i) 
    {
        int s1 = 2*i-1;
        ampo += -1*mu, "Nup", s1;
        ampo += -1*mu, "Ndn", s1;
    }
    //auto HE = MPOT(ampo);

    /////////////////////////////
    //      pairing term       //
    /////////////////////////////
    for (int i=1; i<=N; ++i)
    {
        int s1 = 2*i-1;
        ampo += delta, "Cdagup", s1, "Cdagdn", s1;
        ampo += delta, "Cdn", s1, "Cup", s1;
    }


    auto H = MPOT(ampo);

    ///////////////////////////////////////////////////
    //             total number operator             //
    ///////////////////////////////////////////////////
    auto nmpo = AutoMPO(sites);
    for(int i=1; i<=N; ++i)
    {
        int s1 = 2*i-1;
        nmpo += 1.0, "Nup", s1;
        nmpo += 1.0, "Ndn", s1;
    }

   
    auto Ntot = MPOT(nmpo);


    /////////////////////////////////////////////////////
    //         double occupancy on first site          //
    /////////////////////////////////////////////////////

    auto dompo = AutoMPO(sites);
    dompo += 1.0, "Nupdn", 1;
    auto Docc = MPOT(dompo);

    /////////////////////////////////////////////////////
    //           pairing order on first site           //
    /////////////////////////////////////////////////////

    auto pmpo = AutoMPO(sites);
    for (int i=1; i<=N; ++i)
    {
        int s1 = 2*i-1;
        pmpo += 1., "Cdagup", s1, "Cdagdn", s1;
        pmpo += 1., "Cdn", s1, "Cup", s1;
    }

    auto pairing = MPOT(pmpo);

    
    ////////////////////////////////////////////////////
    //                 time evolution                 //
    ////////////////////////////////////////////////////

    MPOT expHa,expHb;
    MPOT expH;

    if(realstep)
        {
        expH = toExpH<TensorT>(ampo,tau);
        }
    else
        {
        auto taua = tau/2.*(1.+1._i);
        auto taub = tau/2.*(1.-1._i);
        println("Making expHa and expHb");
        expHa = toExpH<TensorT>(ampo,taua);
        expHb = toExpH<TensorT>(ampo,taub);
        }



    //
    // Make initial 'wavefunction' which is a product
    // of perfect singlets between neighboring sites
    //
    auto psi = MPST(sites);
    auto psin = MPST(sites);

    for(int n = 1; n <= 2*N; n += 2)
        {
        auto s1 = sites(n);
        auto s2 = sites(n+1);
        auto wf = TensorT(s1,s2);
        // define the initial state of the real-facticious pair for fermions
        wf.set(s1(1),s2(4), 0.5);
        wf.set(s1(2),s2(3), 0.5);
        wf.set(s1(3),s2(2), 0.5);
        wf.set(s1(4),s2(1), 0.5);
        //Wf.set(s1(1),s2(1), 0.5);
        //Wf.set(s1(2),s2(2), 0.5);
        //Wf.set(s1(3),s2(3), 0.5);
        //Wf.set(s1(4),s2(4), 0.5);
        TensorT D;
        psi.Aref(n) = TensorT(s1);
        psi.Aref(n+1) = TensorT(s2);
        svd(wf,psi.Aref(n),D,psi.Aref(n+1));
        psi.Aref(n) *= D;
        
        psin.Aref(n) = TensorT(s1);
        psin.Aref(n+1) = TensorT(s2);
        svd(wf,psin.Aref(n),D,psin.Aref(n+1));
        psin.Aref(n) *= D;
        }

    auto obs = TStateObserver<TensorT>(psi);

    auto ttotal = beta/2.;
    const int nt = int(ttotal/tau+(1e-9*(ttotal/tau)));
    if(fabs(nt*tau-ttotal) > 1E-9)
        {
        Error("Timestep not commensurate with total time");
        }
    printfln("Doing %d steps of tau=%f",nt,tau);

    auto targs = args;

    //
    // containers to save data
    //
    auto En = Vector(nt);
    auto Don = Vector(nt);
    auto Nn = Vector(nt);
    auto Betas = Vector(nt);

    //
    // imaginary time evolution
    //
    Real tsofar = 0;
    for(int tt = 1; tt <= nt; ++tt)
        {
        //if(realstep)
        //    {
        //    psi = exactApplyMPO(expH,psi,args);
        //    }
        //else
        //    {
        //    psi = exactApplyMPO(expHa,psi,args);
        //    psi = exactApplyMPO(expHb,psi,args);
        //    }
        if(rungekutta)
            {
            if(fitmpo)
                {
                if(tt<2) psi = rk4_exact_1timestep(psi, tau, H, args);
                else psi = rk4_fit_1timestep(psi, tau, H, args);
                }
            else
                {
                psi = rk4_exact_1timestep(psi, tau, H, args);
                }
            }
        // MPO evolution
        else
            {
            if(realstep)
                {
                if(fitmpo)
                    fitApplyMPO(psi,expH,psi,args);
                else
                    psi = exactApplyMPO(expH,psi,args);
                }
            else
                {
                if(fitmpo)
                    {
                    fitApplyMPO(psi,expHa,psi,args);
                    fitApplyMPO(psi,expHb,psi,args);
                    }
                else
                    {
                    psi = exactApplyMPO(expHa,psi,args);
                    psi = exactApplyMPO(expHb,psi,args);
                    }
                }
            }


        psi.Aref(1) /= norm(psi.A(1));
        tsofar += tau;
        targs.add("TimeStepNum",tt);
        targs.add("Time",tsofar);
        targs.add("TotalTime",ttotal);
        obs.measure(targs);

        //Record beta value
        auto bb = (2*tsofar);
        Betas(tt-1) = bb;

        //
        //
        // Measure Energy and
        //
        auto en = overlap(psi,H,psi);
        printfln("###### Energy/N         %.4f  %.12f",bb,en/N);
        En(tt-1) = en/N;
        //
        // Measure total particle number
        //
        auto npart = overlap(psi, Ntot, psi);
        printfln("###### Ntot             %.4f  %.6f", bb, npart);
        Nn(tt-1) = npart;
        //
        // Measure double occupancy
        //
        auto dos = overlap(psi, Docc, psi);
        printfln("###### Double occupancy %.4f  %.12f",bb, dos);
        Don(tt-1) = dos;
        //
        // Measure double occupancy
        //
        auto pairorder = overlap(psi, pairing, psi);
        //psin = exactApplyMPO(pairing,psi,args);
        //auto pairorder = overlap(psi, psin);
        printfln("###### pairing             %.4f  %.6f", bb,pairorder/N);
        }
    // end time evolution

    //
    //write to file
    //
    //std::ofstream enf(outdir+"en_U" + std::to_string(U) + ".dat");
    //std::ofstream npartf(outdir+"npart_U" + std::to_string(U) + ".dat");
    //std::ofstream doccf(outdir+"docc_U" + std::to_string(U) + ".dat");
    //for(auto n : range(Betas))
    //    {
    //    enf << format("%.14f %.14f\n",Betas(n),En(n));
    //    npartf << format("%.14f %.14f\n",Betas(n),Nn(n));
    //    doccf << format("%.14f %.14f\n",Betas(n),Don(n));
    //    }

    //enf.close();
    //npartf.close();
    //doccf.close();

    //writeToFile(outdir+"/chkdr/sites_U"+std::to_string(U),sites);
    //writeToFile(outdir+"/chkdr/psi_U"+std::to_string(U),psi);

    return 0;
    }


MPST rk4_fit_1timestep(MPST psi, double tau, MPOT H, Args args)
    {

    cout << "4th Runge-Kutta with fit MPO\n";
    auto k1 = -tau*fitApplyMPO(psi, H, args);
    auto k2 = -tau*fitApplyMPO(sum(psi, 0.5*k1, args), H, args);
    auto k3 = -tau*fitApplyMPO(sum(psi, 0.5*k2, args), H, args);
    auto k4 = -tau*fitApplyMPO(sum(psi, k3, args), H, args);
    auto terms  = vector<MPST>(5);
    terms.at(0) = psi;
    terms.at(1) = 1./6.* k1;
    terms.at(2) = 1./3.* k2;
    terms.at(3) = 1./3.* k3;
    terms.at(4) = 1./6.* k4;
    psi = sum(terms, args);
    return psi;
    }   

MPST rk4_exact_1timestep(MPST psi, double tau, MPOT H, Args args)
    {
    cout << "4th Runge-Kutta with exact MPO\n";
    auto k1 = -tau*exactApplyMPO(H, psi, args);
    auto k2 = -tau*exactApplyMPO(H, sum(psi, 0.5*k1, args), args);
    auto k3 = -tau*exactApplyMPO(H, sum(psi, 0.5*k2, args), args);
    auto k4 = -tau*exactApplyMPO(H, sum(psi, k3, args), args);
    auto terms  = vector<MPST>(5);
    terms.at(0) = psi;
    terms.at(1) = 1./6.* k1;
    terms.at(2) = 1./3.* k2;
    terms.at(3) = 1./3.* k3;
    terms.at(4) = 1./6.* k4;
    psi = sum(terms, args);
    return psi;
    }
