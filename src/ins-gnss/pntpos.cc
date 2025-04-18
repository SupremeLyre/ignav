/*------------------------------------------------------------------------------
 * pntpos.c : standard positioning
 *
 * version : $Revision:$ $Date:$
 * history : 2010/07/28 1.0  moved from rtkcmn.c
 *                           changed api:
 *                               pntpos()
 *                           deleted api:
 *                               pntvel()
 *           2011/01/12 1.1  add option to include unhealthy satellite
 *                           reject duplicated observation data
 *                           changed api: ionocorr()
 *           2011/11/08 1.2  enable snr mask for single-mode (rtklib_2.4.1_p3)
 *           2012/12/25 1.3  add variable snr mask
 *           2014/05/26 1.4  support galileo and beidou
 *           2015/03/19 1.5  fix bug on ionosphere correction for GLO and BDS
 *-----------------------------------------------------------------------------*/
#include <navlib.h>

/* constants -----------------------------------------------------------------*/
#define MAXITR 10     /* max number of iteration for point pos */
#define MAXVARP 30.0  /* max variance for standard positioning */
#define ERR_ION 5.0   /* ionospheric delay std (m) */
#define ERR_TROP 3.0  /* tropspheric delay std (m) */
#define ERR_SAAS 0.3  /* saastamoinen model error std (m) */
#define ERR_BRDCI 0.5 /* broadcast iono model error factor */
#define ERR_CBIAS 0.3 /* code bias error std (m) */
#define REL_HUMI 0.7  /* relative humidity for saastamoinen model */
#define ADJOBS 0      /* adjust observation data */

/* jacobian of pseudorange measurement by ins attitude error states-----------
 * args  :  double *e    I  line-of-sight vector (ecef)
 *          double *l    I  lever arm of body frame to gps antenna
 *          double *Cbe  I  transform matrix of body frame to ecef
 *          double *dpda O  jacobian of ins attitude error states
 * return : none
 * --------------------------------------------------------------------------*/
static void jacob_dp_da(const double *e, const double *l, const double *Cbe, double *dpda)
{
    double T[9];

    matmul("NN", 3, 1, 3, -1.0, Cbe, l, 0.0, dpda);
    skewsym3(dpda, T);
    matmul("NN", 1, 3, 3, 1.0, e, T, 0.0, dpda);
}
/* jacobian of pseudorange measurement by lever arm---------------------------
 * args  :  double *e    I  line-of-sight vector (ecef)
 *          double *Cbe  I  transform matrix of body frame to ecef
 *          double *dpdl O  jacobian of lever arm
 * return : none
 * --------------------------------------------------------------------------*/
static void jacob_dp_dl(const double *e, const double *Cbe, double *dpdl)
{
    matmul("NN", 1, 3, 3, 1.0, e, Cbe, 0.0, dpdl);
}
/* jacobian of perturb rotation wrt. perturb euler angles--------------------*/
static void jacob_prot_pang(const double *Cbe, double *S)
{
    double rpy[3] = {0};
    c2rpy(Cbe, rpy);
    S[0] = cos(rpy[1]) * cos(rpy[2]);
    S[3] = sin(rpy[2]);
    S[6] = 0.0;
    S[1] = -cos(rpy[1]) * sin(rpy[2]);
    S[4] = cos(rpy[2]);
    S[7] = 0.0;
    S[2] = sin(rpy[1]);
    S[5] = 0.0;
    S[8] = 1.0;
}
/* pseudorange measurement error variance ------------------------------------*/
static double varerr(const prcopt_t *opt, double el, int sys)
{
    double fact, varr;
    fact = sys == SYS_GLO ? EFACT_GLO : (sys == SYS_SBS ? EFACT_SBS : EFACT_GPS);
    varr = SQR(opt->err[0]) * (SQR(opt->err[1]) + SQR(opt->err[2]) / sin(el));
    if (opt->ionoopt == IONOOPT_IFLC)
        varr *= SQR(3.0); /* iono-free */
    return SQR(fact) * varr;
}
/* get tgd parameter (m) -----------------------------------------------------*/
static double gettgd(int sat, const nav_t *nav)
{
    int i;
    for (i = 0; i < nav->n; i++)
    {
        if (nav->eph[i].sat != sat)
            continue;
        return CLIGHT * nav->eph[i].tgd[0];
    }
    return 0.0;
}
/* psendorange with code bias correction -------------------------------------*/
static double prange(const obsd_t *obs, const nav_t *nav, const double *azel, int iter, const prcopt_t *opt,
                     double *var)
{
    const double *lam = nav->lam[obs->sat - 1];
    double PC, P1, P2, P1_P2, P1_C1, P2_C2, gamma;
    int i = 0, j = 1, sys;

    *var = 0.0;

    if (!(sys = satsys(obs->sat, NULL)))
        return 0.0;

    /* L1-L2 for GPS/GLO/QZS, L1-L5 for GAL/SBS */
    if (NFREQ >= 3 && (sys & (SYS_GAL | SYS_SBS)))
        j = 2;
    if (NFREQ < 2 || lam[i] == 0.0 || lam[j] == 0.0)
        return 0.0;

    /* test snr mask */
    if (iter > 0)
    {
        if (testsnr(0, i, azel[1], obs->SNR[i] * 0.25, &opt->snrmask))
        {
            trace(4, "snr mask: %s sat=%2d el=%.1f snr=%.1f\n", time_str(obs->time, 0), obs->sat, azel[1] * R2D,
                  obs->SNR[i] * 0.25);
            return 0.0;
        }
        if (opt->ionoopt == IONOOPT_IFLC)
        {
            if (testsnr(0, j, azel[1], obs->SNR[j] * 0.25, &opt->snrmask))
                return 0.0;
        }
    }
    /* f1^2/f2^2 */
    gamma = SQR(lam[j]) / SQR(lam[i]);

    P1 = obs->P[i];
    P2 = obs->P[j];
    P1_P2 = nav->cbias[obs->sat - 1][0];
    P1_C1 = nav->cbias[obs->sat - 1][1];
    P2_C2 = nav->cbias[obs->sat - 1][2];

    /* if no P1-P2 DCB, use TGD instead */
    if (P1_P2 == 0.0 && (sys & (SYS_GPS | SYS_GAL | SYS_QZS)))
    {
        P1_P2 = (1.0 - gamma) * gettgd(obs->sat, nav);
    }
    if (opt->ionoopt == IONOOPT_IFLC)
    { /* dual-frequency */

        if (P1 == 0.0 || P2 == 0.0)
            return 0.0;
        if (obs->code[i] == CODE_L1C)
            P1 += P1_C1; /* C1->P1 */
        if (obs->code[j] == CODE_L2C)
            P2 += P2_C2; /* C2->P2 */

        /* iono-free combination */
        PC = (gamma * P1 - P2) / (gamma - 1.0);
    }
    else
    { /* single-frequency */
        if (P1 == 0.0)
            return 0.0;
        if (obs->code[i] == CODE_L1C)
            P1 += P1_C1; /* C1->P1 */
        PC = P1 - P1_P2 / (1.0 - gamma);
    }
    if (opt->sateph == EPHOPT_SBAS)
        PC -= P1_C1; /* sbas clock based C1 */

    *var = SQR(ERR_CBIAS);

    return PC;
}
/* ionospheric correction ------------------------------------------------------
 * compute ionospheric correction
 * args   : gtime_t time     I   time
 *          nav_t  *nav      I   navigation data
 *          int    sat       I   satellite number
 *          double *pos      I   receiver position {lat,lon,h} (rad|m)
 *          double *azel     I   azimuth/elevation angle {az,el} (rad)
 *          int    ionoopt   I   ionospheric correction option (IONOOPT_???)
 *          double *ion      O   ionospheric delay (L1) (m)
 *          double *var      O   ionospheric delay (L1) variance (m^2)
 * return : status(1:ok,0:error)
 *-----------------------------------------------------------------------------*/
extern int ionocorr(gtime_t time, const nav_t *nav, int sat, const double *pos, const double *azel, int ionoopt,
                    double *ion, double *var)
{
    trace(4, "ionocorr: time=%s opt=%d sat=%2d pos=%.3f %.3f azel=%.3f %.3f\n", time_str(time, 3), ionoopt, sat,
          pos[0] * R2D, pos[1] * R2D, azel[0] * R2D, azel[1] * R2D);

    /* broadcast model */
    if (ionoopt == IONOOPT_BRDC)
    {
        *ion = ionmodel(time, nav->ion_gps, pos, azel);
        *var = SQR(*ion * ERR_BRDCI);
        return 1;
    }
    /* sbas ionosphere model */
    if (ionoopt == IONOOPT_SBAS)
    {
        return sbsioncorr(time, nav, pos, azel, ion, var);
    }
    /* ionex tec model */
    if (ionoopt == IONOOPT_TEC)
    {
        return iontec(time, nav, pos, azel, 1, ion, var);
    }
    /* qzss broadcast model */
    if (ionoopt == IONOOPT_QZS && norm(nav->ion_qzs, 8) > 0.0)
    {
        *ion = ionmodel(time, nav->ion_qzs, pos, azel);
        *var = SQR(*ion * ERR_BRDCI);
        return 1;
    }
    /* lex ionosphere model */
    if (ionoopt == IONOOPT_LEX)
    {
        return lexioncorr(time, nav, pos, azel, ion, var);
    }
    *ion = 0.0;
    *var = ionoopt == IONOOPT_OFF ? SQR(ERR_ION) : 0.0;
    return 1;
}
/* tropospheric correction -----------------------------------------------------
 * compute tropospheric correction
 * args   : gtime_t time     I   time
 *          nav_t  *nav      I   navigation data
 *          double *pos      I   receiver position {lat,lon,h} (rad|m)
 *          double *azel     I   azimuth/elevation angle {az,el} (rad)
 *          int    tropopt   I   tropospheric correction option (TROPOPT_???)
 *          double *trp      O   tropospheric delay (m)
 *          double *var      O   tropospheric delay variance (m^2)
 * return : status(1:ok,0:error)
 *-----------------------------------------------------------------------------*/
extern int tropcorr(gtime_t time, const nav_t *nav, const double *pos, const double *azel, int tropopt, double *trp,
                    double *var)
{
    trace(4, "tropcorr: time=%s opt=%d pos=%.3f %.3f azel=%.3f %.3f\n", time_str(time, 3), tropopt, pos[0] * R2D,
          pos[1] * R2D, azel[0] * R2D, azel[1] * R2D);

    /* saastamoinen model */
    if (tropopt == TROPOPT_SAAS || tropopt == TROPOPT_EST || tropopt == TROPOPT_ESTG)
    {
        *trp = tropmodel(time, pos, azel, REL_HUMI);
        *var = SQR(ERR_SAAS / (sin(azel[1]) + 0.1));
        return 1;
    }
    /* sbas troposphere model */
    if (tropopt == TROPOPT_SBAS)
    {
        *trp = sbstropcorr(time, pos, azel, var);
        return 1;
    }
    /* no correction */
    *trp = 0.0;
    *var = tropopt == TROPOPT_OFF ? SQR(ERR_TROP) : 0.0;
    return 1;
}
/* imu body position transform to gps antenna---------------------------------*/
extern void insp2antp(const insstate_t *ins, double *rr)
{
    int i;
    double T[3];
    matmul3v("N", ins->Cbe, ins->lever, T);
    for (i = 0; i < 3; i++)
        rr[i] = ins->re[i] + T[i];
}
/* pseudorange residuals -----------------------------------------------------*/
static int rescode(int iter, const obsd_t *obs, int n, const double *rs, const double *dts, const double *vare,
                   const int *svh, const nav_t *nav, double *x, const prcopt_t *opt, const insstate_t *ins, double *v,
                   double *H, double *var, double *azel, int *vsat, double *resp, int *ns)
{
    const insopt_t *iopt = &opt->insopt;
    double r, dion, dtrp, vmeas, vion, vtrp, rr[3], pos[3], e[3], P, lam_L1;
    double dpda[3], dpdl[3], S[9], dpdap[3];
    int i, j, nv = 0, sys, mask[4] = {0}, nx, tc = 0, f;
    int ila = 0, nla = 0, irc = 3, nrc = 0;
    int igl, iga, icp, IP, NP, IA, NA;

    nx = 4 + 3; /* number of estimate state */

    if (ins)
    {
        tc = opt->mode == PMODE_INS_TGNSS && iopt->tc == INSTC_SINGLE;
        nla = xnLa(iopt);
        ila = xiLa(iopt);
        nrc = xnRc(iopt);
        irc = xiRc(iopt);
        IP = xiP(iopt);
        NP = xnP(iopt);
        IA = xiA(iopt);
        NA = xnA(iopt);
        nx = xnX(iopt);
    }
    /* xiRc(insopt)+0: GPS receiver clock
     * xiRc(insopt)+1: GLO receiver clock
     * xiRc(insopt)+2: GAL receiver clock
     * xiRc(insopt)+3: BDS receiver clock
     * */
    igl = nrc ? irc + 1 : 4;
    iga = nrc ? irc + 2 : 5;
    icp = nrc ? irc + 3 : 6; /* receiver clock state index */

    trace(3, "rescode : n=%d\n", n);

    if (ins)
    {
        insp2antp(ins, rr);
        for (i = 0; i < 4; i++)
            x[irc + i] = ins->dtr[i] * CLIGHT;
    }
    else
    {
        for (i = 0; i < 3; i++)
            rr[i] = x[i];
    }
    ecef2pos(rr, pos);

    for (i = *ns = 0; i < n && i < MAXOBS; i++)
    {
        vsat[i] = 0;
        azel[i * 2] = azel[1 + i * 2] = resp[i] = 0.0;

        if (!(sys = satsys(obs[i].sat, NULL)))
            continue;

        /* reject duplicated observation data */
        if (i < n - 1 && i < MAXOBS - 1 && obs[i].sat == obs[i + 1].sat)
        {
            trace(2, "duplicated observation data %s sat=%2d\n", time_str(obs[i].time, 3), obs[i].sat);
            i++;
            continue;
        }
        /* geometric distance/azimuth/elevation angle */
        if ((r = geodist(rs + i * 6, rr, e)) <= 0.0 || satazel(pos, e, azel + i * 2) < opt->elmin)
            continue;

        /* psudorange with code bias correction */
        if ((P = prange(obs + i, nav, azel + i * 2, iter, opt, &vmeas)) == 0.0)
            continue;

        /* excluded satellite? */
        if (satexclude(obs[i].sat, svh[i], opt))
            continue;

        /* ionospheric corrections */
        if (!ionocorr(obs[i].time, nav, obs[i].sat, pos, azel + i * 2, iter > 0 ? opt->ionoopt : IONOOPT_BRDC, &dion,
                      &vion))
            continue;

        /* GPS-L1 -> L1/B1 */
        if ((lam_L1 = nav->lam[obs[i].sat - 1][0]) > 0.0)
        {
            dion *= SQR(lam_L1 / lam_carr[0]);
        }
        /* tropospheric corrections */
        if (!tropcorr(obs[i].time, nav, pos, azel + i * 2, iter > 0 ? opt->tropopt : TROPOPT_SAAS, &dtrp, &vtrp))
        {
            continue;
        }
        /* pseudorange residual */
        v[nv] = P - (r + x[irc] - CLIGHT * dts[i * 2] + dion + dtrp);

        /* design matrix */
        if (tc)
        {
            jacob_dp_da(e, ins->lever, ins->Cbe, dpda);
            jacob_dp_dl(e, ins->Cbe, dpdl);

#if UPD_IN_EULER
            jacob_prot_pang(ins->Cbe, S);
            matcpy(dpdap, dpda, 1, 3);
            matmul("NN", 1, 3, 3, 1.0, dpdap, S, 0.0, dpda);
#endif
            if (nrc)
                H[irc + nv * nx] = 1.0;
            if (nla)
            {
                for (j = ila; j < ila + nla; j++)
                    H[j + nv * nx] = dpdl[j - ila];
            }
            for (j = IP; j < IP + NP; j++)
                H[j + nv * nx] = e[j - IP];
            for (j = IA; j < IA + NA; j++)
                H[j + nv * nx] = dpda[j - IA];
        }
        else
        {
            for (j = 0; j < nx; j++)
                H[j + nv * nx] = j < 3 ? -e[j] : (j == 3 ? 1.0 : 0.0);
        }
        /* time system and receiver bias offset correction */
        if (sys == SYS_GLO)
        {
            v[nv] -= x[igl];
            H[igl + nv * nx] = 1.0;
            mask[1] = 1;
        }
        else if (sys == SYS_GAL)
        {
            v[nv] -= x[iga];
            H[iga + nv * nx] = 1.0;
            mask[2] = 1;
        }
        else if (sys == SYS_CMP)
        {
            v[nv] -= x[icp];
            H[icp + nv * nx] = 1.0;
            mask[3] = 1;
        }
        else
            mask[0] = 1;

        vsat[i] = 1;
        resp[i] = v[nv];
        (*ns)++;

        /* error variance */
        var[nv++] = varerr(opt, azel[1 + i * 2], sys) + vare[i] + vmeas + vion + vtrp;

        trace(4, "sat=%2d azel=%5.1f %4.1f res=%7.3f sig=%5.3f\n", obs[i].sat, azel[i * 2] * R2D, azel[1 + i * 2] * R2D,
              resp[i], sqrt(var[nv - 1]));
    }
    /* constraint to avoid rank-deficient */
    for (i = 0; i < 4; i++)
    {
        if (mask[i])
            continue;
        v[nv] = 0.0;
        if (tc)
        {
            H[irc + i + nx * nv] = 1.0;
        }
        else
        {
            for (j = 0; j < nx; j++)
                H[j + nv * nx] = j == i + 3 ? 1.0 : 0.0;
        }
        var[nv++] = 0.01;
    }
    trace(3, "H=\n");
    tracemat(3, H, nx, nv, 15, 6);
    trace(3, "v=\n");
    tracemat(3, v, nv, 1, 15, 6);
    return nv;
}
/* validate solution ---------------------------------------------------------*/
static int valsol(const double *azel, const int *vsat, int n, const prcopt_t *opt, const double *v, int nv, int nx,
                  char *msg)
{
    double azels[MAXOBS * 2], dop[4], vv;
    int i, ns;

    trace(3, "valsol  : n=%d nv=%d\n", n, nv);

    /* chi-square validation of residuals */
    vv = dot(v, v, nv);
    if (nv > nx && vv > chisqr[nv - nx - 1])
    {
        sprintf(msg, "chi-square error nv=%d vv=%.1f cs=%.1f", nv, vv, chisqr[nv - nx - 1]);
        return 0;
    }
    /* large gdop check */
    for (i = ns = 0; i < n; i++)
    {
        if (!vsat[i])
            continue;
        azels[ns * 2] = azel[i * 2];
        azels[1 + ns * 2] = azel[1 + i * 2];
        ns++;
    }
    dops(ns, azels, opt->elmin, dop);
    if (dop[0] <= 0.0 || dop[0] > opt->maxgdop)
    {
        sprintf(msg, "gdop error nv=%d gdop=%.1f", nv, dop[0]);
        return 0;
    }
    return 1;
}
/* validate solution for ins filter-------------------------------------------*/
static int valins(const double *azel, const int *vsat, int n, const prcopt_t *opt, const double *v, int nv,
                  const double *x, const double *R, double thres, char *msg)
{
    const insopt_t *insopt = &opt->insopt;
    double azels[MAXOBS * 2], dop[4], fact = SQR(thres);
    int i, ns, nba = 0, iba = 0, nbg = 0, ibg = 0;

    trace(3, "valins  : n=%d nv=%d\n", n, nv);

    nba = xnBa(insopt);
    iba = xiBa(insopt);
    nbg = xnBg(insopt);
    ibg = xiBg(insopt);

    /* check estimated states */
    if (norm(x, 3) > 5.0 * D2R || (nba ? norm(x + iba, 3) > 1E4 * Mg2M : false) ||
        (nbg ? norm(x + ibg, 3) > 5.0 * D2R : false))
    {
        trace(2, "too large estimated state error\n");
        return 0;
    }
    /* post-fit residual test */
    for (i = 0; i < nv; i++)
    {
        if (v[i] * v[i] < fact * R[i + i * nv])
            continue;
        trace(2, "large residual (v=%6.3f sig=%.3f)\n", v[i], SQRT(R[i + i * nv]));
    }
    /* large gdop check */
    for (i = ns = 0; i < n; i++)
    {
        if (!vsat[i])
            continue;
        azels[ns * 2] = azel[i * 2];
        azels[1 + ns * 2] = azel[1 + i * 2];
        ns++;
    }
    dops(ns, azels, opt->elmin, dop);
    if (dop[0] <= 0.0 || dop[0] > opt->maxgdop)
    {
        sprintf(msg, "gdop error nv=%d gdop=%.1f", nv, dop[0]);
        return 0;
    }
    return 1;
}
/* ins estimate states by using pseudorange measurement-----------------------*/
static int estinspr(const obsd_t *obs, int n, const double *rs, const double *dts, const double *vare, const int *svh,
                    const nav_t *nav, const prcopt_t *opt, sol_t *sol, insstate_t *ins, double *azel, int *vsat,
                    double *resp, char *msg)
{
    int i, nx, nv, ns, stat = 0, irc = 0, IP;
    double *x, *R, *v, *H, *var, *P;
    const insopt_t *insopt = &opt->insopt;
    static insstate_t inss = {0};

    trace(3, "estinspr:\n");

    nx = xnX(insopt);
    irc = xiRc(insopt);
    IP = xiP(insopt);

    x = zeros(nx, 1);
    R = zeros(NFREQ * n + 4, NFREQ * n + 4);
    H = zeros(nx, NFREQ * n + 4);
    v = zeros(NFREQ * n + 4, 1);
    var = mat(NFREQ * n + 4, 1);
    P = mat(nx, nx);

    /* prefit residuals */
    nv = rescode(1, obs, n, rs, dts, vare, svh, nav, x, opt, ins, v, H, var, azel, vsat, resp, &ns);

    /* tightly coupled */
    if (nv)
    {

        matcpy(P, ins->P, ins->nx, ins->nx);

        /* measurement variance */
        for (i = 0; i < nv; i++)
            R[i + i * nv] = var[i];

        /* ekf filter */
        stat = filter(x, P, H, v, R, nx, nv);

        if (stat)
        {
            sprintf(msg, "ekf filter error info=%d", stat);
            stat = 0;
        }
        else
        {
            inss.re[0] = ins->re[0] - x[IP + 0];
            inss.re[1] = ins->re[1] - x[IP + 1];
            inss.re[2] = ins->re[2] - x[IP + 2];

            for (i = 0; i < 4; i++)
                inss.dtr[i] = x[irc + i] / CLIGHT;

            /* postfit residuals */
            nv = rescode(1, obs, n, rs, dts, vare, svh, nav, x, opt, &inss, v, H, var, azel, vsat, resp, &ns);

            /* valid solutions */
            if (nv && (stat = valins(azel, vsat, n, opt, v, nv, x, R, 4.0, msg)))
            {

                matcpy(ins->P, P, nx, nx);

                /* close loop for ins states */
                clp(ins, insopt, x);

                /* correction for receiver clock */
                for (i = 0; i < 4; i++)
                    ins->dtr[i] = x[irc + i] / CLIGHT;

                ins->ns = (unsigned char)ns;
                ins->age = 0.0;
                ins->gstat = opt->sateph == EPHOPT_SBAS ? SOLQ_SBAS : SOLQ_SINGLE;
            }
            else
            {
                trace(2, "tightly coupled valid solutions fail\n");
                stat = 0;
            }
        }
    }
    else
    {
        trace(2, "no observation data\n");
        stat = 0;
    }
    free(v);
    free(H);
    free(var);
    free(x);
    free(R);
    free(P);
    return stat;
}
/* estimate receiver position ------------------------------------------------*/
static int estpos(const obsd_t *obs, int n, const double *rs, const double *dts, const double *vare, const int *svh,
                  const nav_t *nav, const prcopt_t *opt, sol_t *sol, double *azel, int *vsat, double *resp, char *msg)
{
    int i, j, k, info, stat, nv = 0, ns, nx = 4 + 3;
    double *x, *dx, *Q, *v, *H, *var, sig;

    trace(3, "estpos  : n=%d\n", n);

    x = zeros(nx, 1);
    dx = zeros(nx, 1);
    Q = zeros(nx, nx);
    v = mat(n + 4, 1);
    H = mat(nx, n + 4);
    var = mat(n + 4, 1);

    for (i = 0; i < 3; i++)
        x[i] = sol->rr[i];

    for (i = 0; i < MAXITR; i++)
    {

        /* pseudorange residuals */
        nv = rescode(i, obs, n, rs, dts, vare, svh, nav, x, opt, NULL, v, H, var, azel, vsat, resp, &ns);

        if (nv < nx)
        {
            sprintf(msg, "lack of valid sats ns=%d", nv);
            break;
        }
        /* weight by variance */
        for (j = 0; j < nv; j++)
        {
            sig = sqrt(var[j]);
            v[j] /= sig;
            for (k = 0; k < nx; k++)
                H[k + j * nx] /= sig;
        }
        /* least square estimation */
        if ((info = lsq(H, v, nx, nv, dx, Q)))
        {
            sprintf(msg, "lsq error info=%d", info);
            break;
        }
        for (j = 0; j < nx; j++)
            x[j] += dx[j];

        if (norm(dx, nx) < 1E-4)
        {
            sol->type = 0;
            sol->time = timeadd(obs[0].time, -x[3] / CLIGHT);
            sol->dtr[0] = x[3] / CLIGHT; /* receiver clock bias (s) */
            sol->dtr[1] = x[4] / CLIGHT; /* glo-gps time offset (s) */
            sol->dtr[2] = x[5] / CLIGHT; /* gal-gps time offset (s) */
            sol->dtr[3] = x[6] / CLIGHT; /* bds-gps time offset (s) */
            for (j = 0; j < 6; j++)
                sol->rr[j] = j < 3 ? x[j] : 0.0;
            for (j = 0; j < 3; j++)
                sol->qr[j] = (float)Q[j + j * nx];
            for (j = 0; j < 3; j++)
                var[j] = SQRT(Q[j + j * nx]);
            sol->qr[3] = (float)Q[1];      /* cov xy */
            sol->qr[4] = (float)Q[2 + nx]; /* cov yz */
            sol->qr[5] = (float)Q[2];      /* cov zx */
            sol->ns = (unsigned char)ns;
            sol->age = sol->ratio = 0.0;

            trace(3, "receiver position=%10.6lf  %10.6lf  %10.6lf\n", sol->rr[0], sol->rr[1], sol->rr[2]);

            /* validate solution */
            if ((stat = (valsol(azel, vsat, n, opt, v, nv, nx, msg) && norm(var, 3) < MAXVARP)))
            {
                sol->stat = opt->sateph == EPHOPT_SBAS ? SOLQ_SBAS : SOLQ_SINGLE;
            }
            free(v);
            free(H);
            free(var);
            free(x);
            free(dx);
            free(Q);

            return stat;
        }
    }
    if (i >= MAXITR)
        sprintf(msg, "iteration divergent i=%d", i);

    free(v);
    free(H);
    free(var);
    free(x);
    free(dx);
    free(Q);

    /* tightly-coupled mode always is available
     * as long as have at least a satellite
     * */
    if (nv < nx && opt->mode == PMODE_INS_TGNSS)
    {
        return nv;
    }
    return 0;
}
/* raim fde (failure detection and exclution) -------------------------------*/
static int raim_fde(const obsd_t *obs, int n, const double *rs, const double *dts, const double *vare, const int *svh,
                    const nav_t *nav, const prcopt_t *opt, sol_t *sol, double *azel, int *vsat, double *resp, char *msg)
{
    obsd_t *obs_e;
    sol_t sol_e = {{0}};
    char tstr[32], name[16], msg_e[128];
    double *rs_e, *dts_e, *vare_e, *azel_e, *resp_e, rms_e, rms = 100.0;
    int i, j, k, nvsat, stat = 0, *svh_e, *vsat_e, sat = 0;

    trace(3, "raim_fde: %s n=%2d\n", time_str(obs[0].time, 0), n);

    if (!(obs_e = (obsd_t *)malloc(sizeof(obsd_t) * n)))
        return 0;
    rs_e = mat(6, n);
    dts_e = mat(2, n);
    vare_e = mat(1, n);
    azel_e = zeros(2, n);
    svh_e = imat(1, n);
    vsat_e = imat(1, n);
    resp_e = mat(1, n);

    for (i = 0; i < n; i++)
    {

        /* satellite exclution */
        for (j = k = 0; j < n; j++)
        {
            if (j == i)
                continue;
            obs_e[k] = obs[j];
            matcpy(rs_e + 6 * k, rs + 6 * j, 6, 1);
            matcpy(dts_e + 2 * k, dts + 2 * j, 2, 1);
            vare_e[k] = vare[j];
            svh_e[k++] = svh[j];
        }
        /* estimate receiver position without a satellite */
        if (!estpos(obs_e, n - 1, rs_e, dts_e, vare_e, svh_e, nav, opt, &sol_e, azel_e, vsat_e, resp_e, msg_e))
        {
            trace(3, "raim_fde: exsat=%2d (%s)\n", obs[i].sat, msg);
            continue;
        }
        for (j = nvsat = 0, rms_e = 0.0; j < n - 1; j++)
        {
            if (!vsat_e[j])
                continue;
            rms_e += SQR(resp_e[j]);
            nvsat++;
        }
        if (nvsat < 5)
        {
            trace(3, "raim_fde: exsat=%2d lack of satellites nvsat=%2d\n", obs[i].sat, nvsat);
            continue;
        }
        rms_e = sqrt(rms_e / nvsat);

        trace(3, "raim_fde: exsat=%2d rms=%8.3f\n", obs[i].sat, rms_e);

        if (rms_e > rms)
            continue;

        /* save result */
        for (j = k = 0; j < n; j++)
        {
            if (j == i)
                continue;
            matcpy(azel + 2 * j, azel_e + 2 * k, 2, 1);
            vsat[j] = vsat_e[k];
            resp[j] = resp_e[k++];
        }
        stat = 1;
        *sol = sol_e;
        sat = obs[i].sat;
        rms = rms_e;
        vsat[i] = 0;
        strcpy(msg, msg_e);
    }
    if (stat)
    {
        time2str(obs[0].time, tstr, 2);
        satno2id(sat, name);
        trace(2, "%s: %s excluded by raim\n", tstr + 11, name);
    }
    free(obs_e);
    free(rs_e);
    free(dts_e);
    free(vare_e);
    free(azel_e);
    free(svh_e);
    free(vsat_e);
    free(resp_e);
    return stat;
}
/* doppler residuals ---------------------------------------------------------*/
static int resdop(const obsd_t *obs, int n, const double *rs, const double *dts, const nav_t *nav, const double *rr,
                  const double *x, const double *azel, const int *vsat, double *v, double *H)
{
    double lam, rate, pos[3], E[9], a[3], e[3], vs[3], cosel;
    int i, j, nv = 0;

    trace(3, "resdop  : n=%d\n", n);

    ecef2pos(rr, pos);
    xyz2enu(pos, E);

    for (i = 0; i < n && i < MAXOBS; i++)
    {

        lam = nav->lam[obs[i].sat - 1][0];
        if (obs[i].D[0] == 0.0 || lam == 0.0 || !vsat[i] || norm(rs + 3 + i * 6, 3) <= 0.0)
        {
            continue;
        }
        /* line-of-sight vector in ecef */
        cosel = cos(azel[1 + i * 2]);
        a[0] = sin(azel[i * 2]) * cosel;
        a[1] = cos(azel[i * 2]) * cosel;
        a[2] = sin(azel[1 + i * 2]);
        matmul("TN", 3, 1, 3, 1.0, E, a, 0.0, e);

        /* satellite velocity relative to receiver in ecef */
        for (j = 0; j < 3; j++)
            vs[j] = rs[j + 3 + i * 6] - x[j];

        /* range rate with earth rotation correction */
        rate =
            dot(vs, e, 3) +
            OMGE / CLIGHT * (rs[4 + i * 6] * rr[0] + rs[1 + i * 6] * x[0] - rs[3 + i * 6] * rr[1] - rs[i * 6] * x[1]);

        /* doppler residual */
        v[nv] = -lam * obs[i].D[0] - (rate + x[3] - CLIGHT * dts[1 + i * 2]);

        /* design matrix */
        for (j = 0; j < 4; j++)
            H[j + nv * 4] = j < 3 ? -e[j] : 1.0;

        nv++;
    }
    return nv;
}
/* estimate receiver velocity ------------------------------------------------*/
static void estvel(const obsd_t *obs, int n, const double *rs, const double *dts, const nav_t *nav, const prcopt_t *opt,
                   sol_t *sol, const double *azel, const int *vsat)
{
    double x[4] = {0}, dx[4], Q[16], *v, *H;
    int i, j, nv;

    trace(3, "estvel  : n=%d\n", n);

    v = mat(n, 1);
    H = mat(4, n);

    for (i = 0; i < MAXITR; i++)
    {

        /* doppler residuals */
        if ((nv = resdop(obs, n, rs, dts, nav, sol->rr, x, azel, vsat, v, H)) < 4)
        {
            break;
        }
        /* least square estimation */
        if (lsq(H, v, 4, nv, dx, Q))
            break;

        for (j = 0; j < 4; j++)
            x[j] += dx[j];
        if (norm(dx, 4) < 1E-6)
        {
            for (i = 0; i < 3; i++)
                sol->rr[i + 3] = x[i];
            sol->dtrr = x[3];
            break;
        }
    }
    free(v);
    free(H);
}
/* single-point positioning ----------------------------------------------------
 * compute receiver position, velocity, clock bias by single-point positioning
 * with pseudorange and doppler observables
 * args   : obsd_t *obs      I   observation data
 *          int    n         I   number of observation data
 *          nav_t  *nav      I   navigation data
 *          prcopt_t *opt    I   processing options
 *          sol_t  *sol      IO  solution
 *          insstate_t *ins  IO  ins states
 *          double *azel     IO  azimuth/elevation angle (rad) (NULL: no output)
 *          ssat_t *ssat     IO  satellite status              (NULL: no output)
 *          char   *msg      O   error message for error exit
 * return : status(1:ok,0:error)
 * notes  : assuming sbas-gps, galileo-gps, qzss-gps, compass-gps time offset and
 *          receiver bias are negligible (only involving glonass-gps time offset
 *          and receiver bias)
 *-----------------------------------------------------------------------------*/
extern int pntpos(const obsd_t *obs, int n, const nav_t *nav, const prcopt_t *opt, sol_t *sol, insstate_t *ins,
                  double *azel, ssat_t *ssat, char *msg)
{
    prcopt_t opt_ = *opt;
    double *rs, *dts, *var, *azel_, *resp;
    int i, tc = 0, stat, vsat[MAXOBS] = {0}, svh[MAXOBS];

    sol->stat = SOLQ_NONE;
    tc = opt_.mode == PMODE_INS_TGNSS && opt_.insopt.tc >= INSTC_SINGLE && ins;

    if (n <= 0)
    {
        strcpy(msg, "no observation data");
        return 0;
    }
    trace(3, "pntpos  : tobs=%s n=%d\n", time_str(obs[0].time, 3), n);

    trace(4, "obs=\n");
    traceobs(4, obs, n);
    trace(5, "nav=\n");
    tracenav(5, nav);

    sol->time = obs[0].time;
    msg[0] = '\0';

    rs = mat(6, n);
    dts = mat(2, n);
    var = mat(1, n);
    azel_ = zeros(2, n);
    resp = mat(1, n);

    if (opt_.mode != PMODE_SINGLE)
    { /* for precise positioning */
#if 0
        opt_.sateph =EPHOPT_BRDC;
#endif
        opt_.ionoopt = IONOOPT_BRDC;
        opt_.tropopt = TROPOPT_SAAS;
    }
    /* satellite positons, velocities and clocks */
    satposs(sol->time, obs, n, nav, opt_.sateph, rs, dts, var, svh);

    /* estimate receiver position with pseudorange */
    if (tc)
    {
        stat = estinspr(obs, n, rs, dts, var, svh, nav, &opt_, sol, ins, azel_, vsat, resp, msg); /* tightly coupled */
    }
    else
    {
        stat = estpos(obs, n, rs, dts, var, svh, nav, &opt_, sol, azel_, vsat, resp, msg); /* common single position */
    }
    /* raim fde */
    if (!stat && n >= 6 && opt->posopt[4])
    {
        stat = raim_fde(obs, n, rs, dts, var, svh, nav, &opt_, sol, azel_, vsat, resp, msg);
    }
    /* estimate receiver velocity with doppler */
    if (stat)
        estvel(obs, n, rs, dts, nav, &opt_, sol, azel_, vsat);

    if (azel)
    {
        for (i = 0; i < n * 2; i++)
            azel[i] = azel_[i];
    }
    if (ssat)
    {
        for (i = 0; i < MAXSAT; i++)
        {
            ssat[i].vs = 0;
            ssat[i].azel[0] = ssat[i].azel[1] = 0.0;
            ssat[i].resp[0] = ssat[i].resc[0] = 0.0;
            ssat[i].snr[0] = 0;
        }
        for (i = 0; i < n; i++)
        {
            ssat[obs[i].sat - 1].azel[0] = azel_[i * 2];
            ssat[obs[i].sat - 1].azel[1] = azel_[1 + i * 2];
            ssat[obs[i].sat - 1].snr[0] = obs[i].SNR[0];
            if (!vsat[i])
                continue;
            ssat[obs[i].sat - 1].vs = 1;
            ssat[obs[i].sat - 1].resp[0] = resp[i];
        }
    }
    free(rs);
    free(dts);
    free(var);
    free(azel_);
    free(resp);
    return stat;
}
