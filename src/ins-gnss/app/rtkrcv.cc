/*------------------------------------------------------------------------------
 * rtkrcv.cc : rtk-gps/gnss receiver console app
 *
 * notes   :
 *     current version does not support win32 without pthread library
 *
 * version : $Revision:$ $Date:$
 * history : 2009/12/13 1.0  new
 *           2010/07/18 1.1  add option -m
 *           2010/08/12 1.2  fix bug on ftp/http
 *           2011/01/22 1.3  add option misc-proxyaddr,misc-fswapmargin
 *           2011/08/19 1.4  fix bug on size of arg solopt arg for rtksvrstart()
 *           2012/11/03 1.5  fix bug on setting output format
 *           2013/06/30 1.6  add "nvs" option for inpstr*-format
 *           2014/02/10 1.7  fix bug on printing obs data
 *                           add print of status, glonass nav data
 *                           ignore SIGHUP
 *           2014/04/27 1.8  add "binex" option for inpstr*-format
 *           2014/08/10 1.9  fix cpu overload with abnormal telnet shutdown
 *           2014/08/26 1.10 support input format "rt17"
 *                           change file paths of solution status and debug trace
 *           2015/01/10 1.11 add line editting and command history
 *                           separate codes for virtual console to vt.c
 *           2015/05/22 1.12 fix bug on sp3 id in inpstr*-format options
 *           2015/07/31 1.13 accept 4:stat for outstr1-format or outstr2-format
 *                           add reading satellite dcb
 *           2015/12/14 1.14 add option -sta for station name (#339)
 *           2015/12/25 1.15 fix bug on -sta option (#339)
 *           2015/01/26 1.16 support septentrio
 *           2016/07/01 1.17 support CMR/CMR+
 *           2016/08/20 1.18 add output of patch level with version
 *           2016/09/05 1.19 support ntrip caster for output stream
 *           2016/09/19 1.20 support multiple remote console connections
 *                           add option -w
 *           2017/09/01 1.21 add command ssr
 *-----------------------------------------------------------------------------*/
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <navlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define PRGNAME "rtkrcv"                    /* program name */
#define CMDPROMPT "rtkrcv> "                /* command prompt */
#define MAXCON 32                           /* max number of consoles */
#define MAXARG 10                           /* max number of args in a command */
#define MAXCMD 256                          /* max length of a command */
#define MAXSTR 1024                         /* max length of a stream */
#define OPTSDIR "."                         /* default config directory */
#define OPTSFILE "rtkrcv.conf"              /* default config file */
#define NAVIFILE "rtkrcv.nav"               /* navigation save file */
#define STATFILE "rtkrcv_%Y%m%d%h%M.stat"   /* solution status file */
#define TRACEFILE "rtkrcv_%Y%m%d%h%M.trace" /* debug trace file */
#define INTKEEPALIVE 1000                   /* keep alive interval (ms) */
#define OPENPLOT 0                          /* real time plot for solutions */

#define ESC_CLEAR "\033[H\033[2J" /* ansi/vt100 escape: erase screen */
#define ESC_RESET "\033[0m"       /* ansi/vt100: reset attribute */
#define ESC_BOLD "\033[1m"        /* ansi/vt100: bold */
#define DEBUG 1

/* type defintions -----------------------------------------------------------*/
typedef struct
{                     /* console type */
    int state;        /* state (0:stop,1:run) */
    vt_t *vt;         /* virtual terminal */
    pthread_t thread; /* console thread */
} con_t;

/* function prototypes -------------------------------------------------------*/
extern FILE *popen(const char *, const char *);
extern int pclose(FILE *);

/* global variables ----------------------------------------------------------*/
static rtksvr_t svr = {0};    /* rtk server struct */
static stream_t moni = {0};   /* monitor stream */
static stream_t gtmoni = {0}; /* ground truth monitor stream */

static int intflg = 0; /* interrupt flag (2:shutdown) */

static char passwd[MAXSTR] = "admin"; /* login password */
static int timetype = 0;              /* time format (0:gpst,1:utc,2:jst,3:tow) */
static int soltype = 0;               /* sol format (0:dms,1:deg,2:xyz,3:enu,4:pyl) */
static int solflag = 2;               /* sol flag (1:std+2:age/ratio/ns) */
static int strtype[] = {              /* stream types */
                        STR_SERIAL, STR_NONE, STR_NONE, STR_NONE, STR_NONE, STR_NONE, STR_NONE,
                        STR_NONE,   STR_NONE, STR_NONE, STR_NONE, STR_NONE, STR_NONE, STR_NONE};
static char strpath[14][MAXSTR] = {"", "", "", "", "", "", "", "", "", "", "", "", "", ""}; /* stream paths */
static int strfmt[] = {                                                                     /* stream formats */
                       STRFMT_UBX, STRFMT_RTCM3, STRFMT_SP3, STRFMT_UBXM8, STRFMT_UBXSOL,
                       SOLF_LLH,   SOLF_NMEA,    SOLF_LLH,   SOLF_LLH};
static int svrcycle = 10;              /* server cycle (ms) */
static int timeout = 10000;            /* timeout time (ms) */
static int reconnect = 10000;          /* reconnect interval (ms) */
static int nmeacycle = 5000;           /* nmea request cycle (ms) */
static int buffsize = 5000;            /* input buffer size (bytes) */
static int navmsgsel = 0;              /* navigation mesaage select */
static char proxyaddr[256] = "";       /* http/ntrip proxy */
static int nmeareq = 0;                /* nmea request type (0:off,1:lat/lon,2:single) */
static double nmeapos[] = {0, 0, 0};   /* nmea position (lat/lon/height) (deg,m) */
static char rcvcmds[7][MAXSTR] = {""}; /* receiver commands files */
static char startcmd[MAXSTR] = "";     /* start command */
static char stopcmd[MAXSTR] = "";      /* stop command */
static int modflgr[256] = {0};         /* modified flags of receiver options */
static int modflgs[256] = {0};         /* modified flags of system options */
static int moniport = 0;               /* monitor port */
static int gtmoniport = 0;             /* ground truth monitor port */
static int keepalive = 0;              /* keep alive flag */
static int keepalivegt = 0;            /* keep alive flag for ground truth monitor port */
static int fswapmargin = 30;           /* file swap margin (s) */
static char sta_name[256] = "";        /* station name */

static prcopt_t prcopt;            /* processing options */
static solopt_t solopt[2] = {{0}}; /* solution options */
static filopt_t filopt = {""};     /* file options */

/* help text -----------------------------------------------------------------*/
static const char *usage[] = {"usage: rtkrcv [-s][-p port][-d dev][-o file][-w pwd][-r level][-t level][-sta sta]",
                              "options",
                              "  -s         start RTK server on program startup",
                              "  -p port    port number for telnet console",
                              "  -m port    port number for monitor stream",
                              "  -d dev     terminal device for console",
                              "  -o file    processing options file",
                              "  -w pwd     login password for remote console (\"\": no password)",
                              "  -r level   output solution status file (0:off,1:states,2:residuals)",
                              "  -t level   debug trace level (0:off,1-5:on)",
                              "  -sta sta   station name for receiver dcb",
                              "  -pause     pause program,mainly use in post-process",
                              "  -resume    resume program",
                              "  -reinit    re-initial ins states"};
static const char *helptxt[] = {"start                 : start rtk server",
                                "stop                  : stop rtk server",
                                "restart               : restart rtk sever",
                                "solution [cycle]      : show solution",
                                "status [cycle]        : show rtk status",
                                "satellite [-n] [cycle]: show satellite status",
                                "observ [-n] [cycle]   : show observation data",
                                "navidata [cycle]      : show navigation data",
                                "stream [cycle]        : show stream status",
                                "ssr [cycle]           : show ssr corrections",
                                "error                 : show error/warning messages",
                                "option [opt]          : show option(s)",
                                "set opt [val]         : set option",
                                "load [file]           : load options from file",
                                "save [file]           : save options to file",
                                "log [file|off]        : start/stop log to file",
                                "help|? [path]         : print help",
                                "exit|ctr-D            : logout console (only for telnet)",
                                "shutdown              : shutdown rtk server",
                                "imudata               : imu measurement data",
                                "!command [arg...]     : execute command in shell",
                                "pause                 : pause program",
                                "resume                : resume program",
                                "reinit                : re-initial ins states",
                                ""};
static const char *pathopts[] = {/* path options help */
                                 "stream path formats",
                                 "serial   : port[:bit_rate[:byte[:parity(n|o|e)[:stopb[:fctr(off|on)]]]]]",
                                 "file     : path[::T[::+offset][::xspeed]]",
                                 "tcpsvr   : :port",
                                 "tcpcli   : addr:port",
                                 "ntripsvr : user:passwd@addr:port/mntpnt[:str]",
                                 "ntripcli : user:passwd@addr:port/mntpnt",
                                 "ntripc_s : :passwd@:port",
                                 "ntripc_c : user:passwd@:port",
                                 "ftp      : user:passwd@addr/path[::T=poff,tint,off,rint]",
                                 "http     : addr/path[::T=poff,tint,off,rint]",
                                 ""};
/* receiver options table ----------------------------------------------------*/
#define TIMOPT "0:gpst,1:utc,2:jst,3:tow"
#define CONOPT "0:dms,1:deg,2:xyz,3:enu,4:pyl"
#define FLGOPT "0:off,1:std+2:age/ratio/ns"
#define ISTOPT "0:off,1:serial,2:file,3:tcpsvr,4:tcpcli,7:ntripcli,8:ftp,9:http"
#define OSTOPT "0:off,1:serial,2:file,3:tcpsvr,4:tcpcli,6:ntripsvr,11:ntripc_c"
#define FMTOPT                                                                                                         \
    "0:rtcm2,1:rtcm3,2:oem4,3:oem3,4:ubx,5:ss2,6:hemis,7:skytraq,8:gw10,9:javad,10:nvs,11:binex,12:rt17,13:sbf,14:"    \
    "cmr,15:tersus,18:sp3,19:rnxclk,20:sbas,21:nmea,22:gsof,23:ublox-evk-m8u,24:ublox-sol,25:m39,26:rinex,27:m39-mix," \
    "28:euroc-imu,29:euroc-img,30:karl-img,31:malaga-gnss,32:malaga-imu,33:malaga-img,34:oem6-sol,35:oem6-pose,36:"    \
    "oem6-raw"
#define NMEOPT "0:off,1:latlon,2:single"
#define SOLOPT "0:llh,1:xyz,2:enu,3:nmea,4:stat,5:gsif,6:ins"
#define MSGOPT "0:all,1:rover,2:base,3:corr"

static opt_t rcvopts[] = {{"console-passwd", 2, (void *)passwd, ""},
                          {"console-timetype", 3, (void *)&timetype, TIMOPT},
                          {"console-soltype", 3, (void *)&soltype, CONOPT},
                          {"console-solflag", 0, (void *)&solflag, FLGOPT},

                          {"inpstr1-type", 3, (void *)&strtype[0], ISTOPT},
                          {"inpstr2-type", 3, (void *)&strtype[1], ISTOPT},
                          {"inpstr3-type", 3, (void *)&strtype[2], ISTOPT},
                          {"inpstr4-type", 3, (void *)&strtype[3], ISTOPT},
                          {"inpstr5-type", 3, (void *)&strtype[4], ISTOPT},
                          {"inpstr6-type", 3, (void *)&strtype[5], ISTOPT},
                          {"inpstr7-type", 3, (void *)&strtype[6], ISTOPT},
                          {"inpstr1-path", 2, (void *)strpath[0], ""},
                          {"inpstr2-path", 2, (void *)strpath[1], ""},
                          {"inpstr3-path", 2, (void *)strpath[2], ""},
                          {"inpstr4-path", 2, (void *)strpath[3], ""},
                          {"inpstr5-path", 2, (void *)strpath[4], ""},
                          {"inpstr6-path", 2, (void *)strpath[5], ""},
                          {"inpstr7-path", 2, (void *)strpath[6], ""},
                          {"inpstr1-format", 3, (void *)&strfmt[0], FMTOPT},
                          {"inpstr2-format", 3, (void *)&strfmt[1], FMTOPT},
                          {"inpstr3-format", 3, (void *)&strfmt[2], FMTOPT},
                          {"inpstr4-format", 3, (void *)&strfmt[3], FMTOPT},
                          {"inpstr5-format", 3, (void *)&strfmt[4], FMTOPT},
                          {"inpstr6-format", 3, (void *)&strfmt[5], FMTOPT},
                          {"inpstr7-format", 3, (void *)&strfmt[6], FMTOPT},
                          {"inpstr2-nmeareq", 3, (void *)&nmeareq, NMEOPT},
                          {"inpstr2-nmealat", 1, (void *)&nmeapos[0], "deg"},
                          {"inpstr2-nmealon", 1, (void *)&nmeapos[1], "deg"},
                          {"inpstr2-nmeahgt", 1, (void *)&nmeapos[2], "m"},
                          {"outstr1-type", 3, (void *)&strtype[7], OSTOPT},
                          {"outstr2-type", 3, (void *)&strtype[8], OSTOPT},
                          {"outstr1-path", 2, (void *)strpath[7], ""},
                          {"outstr2-path", 2, (void *)strpath[8], ""},
                          {"outstr1-format", 3, (void *)&strfmt[7], SOLOPT},
                          {"outstr2-format", 3, (void *)&strfmt[8], SOLOPT},
                          {"logstr1-type", 3, (void *)&strtype[9], OSTOPT},
                          {"logstr2-type", 3, (void *)&strtype[10], OSTOPT},
                          {"logstr3-type", 3, (void *)&strtype[11], OSTOPT},
                          {"logstr4-type", 3, (void *)&strtype[12], OSTOPT},
                          {"logstr5-type", 3, (void *)&strtype[13], OSTOPT},
                          {"logstr1-path", 2, (void *)strpath[9], ""},
                          {"logstr2-path", 2, (void *)strpath[10], ""},
                          {"logstr3-path", 2, (void *)strpath[11], ""},
                          {"logstr4-path", 2, (void *)strpath[12], ""},
                          {"logstr5-path", 2, (void *)strpath[13], ""},

                          {"misc-svrcycle", 0, (void *)&svrcycle, "ms"},
                          {"misc-timeout", 0, (void *)&timeout, "ms"},
                          {"misc-reconnect", 0, (void *)&reconnect, "ms"},
                          {"misc-nmeacycle", 0, (void *)&nmeacycle, "ms"},
                          {"misc-buffsize", 0, (void *)&buffsize, "bytes"},
                          {"misc-navmsgsel", 3, (void *)&navmsgsel, MSGOPT},
                          {"misc-proxyaddr", 2, (void *)proxyaddr, ""},
                          {"misc-fswapmargin", 0, (void *)&fswapmargin, "s"},

                          {"misc-startcmd", 2, (void *)startcmd, ""},
                          {"misc-stopcmd", 2, (void *)stopcmd, ""},

                          {"file-cmdfile1", 2, (void *)rcvcmds[0], ""},
                          {"file-cmdfile2", 2, (void *)rcvcmds[1], ""},
                          {"file-cmdfile3", 2, (void *)rcvcmds[2], ""},

                          {"", 0, NULL, ""}};
/* input stream path args------------------------------------------------------
 * inpstr1-path:  rover observation data file path
 * inpstr2-path:  base observation data file path
 * inpstr3-path:  correction data file path
 * inpstr4-path:  gnss position measurement data file path
 * inpstr5-path:  imu measurement raw data file path
 * inpstr6-path:  reserve
 * inpstr7-path:  dual-ants measurement data path
 * ---------------------------------------------------------------------------*/

/* print usage ---------------------------------------------------------------*/
static void printusage(void)
{
    int i;
    for (i = 0; i < (int)(sizeof(usage) / sizeof(*usage)); i++)
    {
        fprintf(stderr, "%s\n", usage[i]);
    }
    exit(0);
}
/* external stop signal ------------------------------------------------------*/
static void sigshut(int sig)
{
    trace(3, "sigshut: sig=%d\n", sig);

    intflg = 1;
}
/* discard space characters at tail ------------------------------------------*/
static void chop(char *str)
{
    char *p;
    for (p = str + strlen(str) - 1; p >= str && !isgraph((int)*p); p--)
        *p = '\0';
}
/* thread to send keep alive for monitor port --------------------------------*/
static void *sendkeepalive(void *arg)
{
    trace(3, "sendkeepalive: start\n");

    while (keepalivegt)
    {
        strwrite(&moni, (unsigned char *)"\r", 1);
        sleepms(INTKEEPALIVE);
    }
    trace(3, "sendkeepalive: stop\n");
    return NULL;
}
/* thread to send keep alive for monitor port --------------------------------*/
static void *sendkeepalive_gt(void *arg)
{
    trace(3, "sendkeepalive_gt: start\n");

    while (keepalive)
    {
        strwrite(&gtmoni, (unsigned char *)"\r", 1);
        sleepms(INTKEEPALIVE);
    }
    trace(3, "sendkeepalive_gt: stop\n");
    return NULL;
}
/* open monitor port ---------------------------------------------------------*/
static int openmoni(int port)
{
    pthread_t thread;
    char path[64];

    trace(3, "openmomi: port=%d\n", port);

    sprintf(path, ":%d", port);

    if (!stropen(&moni, STR_TCPSVR, STR_MODE_RW, path))
        return 0;
    strsettimeout(&moni, timeout, reconnect);
    keepalive = 1;
    pthread_create(&thread, NULL, sendkeepalive, NULL);
    return 1;
}
/* open monitor port ---------------------------------------------------------*/
static int open_gtmoni(int port)
{
    pthread_t thread;
    char path[64];

    trace(3, "openmomi: port=%d\n", port);

    sprintf(path, ":%d", port);

    if (!stropen(&gtmoni, STR_TCPSVR, STR_MODE_RW, path))
        return 0;
    strsettimeout(&gtmoni, timeout, reconnect);
    keepalive = 1;
    pthread_create(&thread, NULL, sendkeepalive_gt, NULL);
    return 1;
}
/* close monitor port --------------------------------------------------------*/
static void closemoni(void)
{
    trace(3, "closemoni:\n");
    keepalive = 0;

    /* send disconnect message */
    strwrite(&moni, (unsigned char *)MSG_DISCONN, strlen(MSG_DISCONN));

    /* wait fin from clients */
    sleepms(1000);
    strclose(&moni);
}
/* close monitor port --------------------------------------------------------*/
static void closemoni_gt(void)
{
    trace(3, "closemoni_gt:\n");
    keepalivegt = 0;

    /* send disconnect message */
    strwrite(&gtmoni, (unsigned char *)MSG_DISCONN, strlen(MSG_DISCONN));

    /* wait fin from clients */
    sleepms(1000);
    strclose(&gtmoni);
}
/* confirm overwrite ---------------------------------------------------------*/
static int confwrite(vt_t *vt, const char *file)
{
    FILE *fp;
    char buff[MAXSTR], *p;

    strcpy(buff, file);
    if ((p = strstr(buff, "::")))
        *p = '\0'; /* omit options in path */
    if (!vt->state || !(fp = fopen(buff, "r")))
        return 1; /* no existing file */
    fclose(fp);

#if DEBUG
    buff[0] = 'y';
#else
    vt_printf(vt, "overwrite %-16s ? (y/n): ", buff);
    if (!vt_gets(vt, buff, sizeof(buff)) || vt->brk)
        return 0;
#endif
    return toupper((int)buff[0]) == 'Y';
}
/* login ---------------------------------------------------------------------*/
static int login(vt_t *vt)
{
    char buff[256];

    trace(3, "login: passwd=%s type=%d\n", passwd, vt->type);

    if (!*passwd || !vt->type)
        return 1;

    while (!(intflg & 2))
    {
        if (!vt_printf(vt, "password: ", PRGNAME))
            return 0;
        vt->blind = 1;
        if (!vt_gets(vt, buff, sizeof(buff)) || vt->brk)
        {
            vt->blind = 0;
            return 0;
        }
        vt->blind = 0;
        if (!strcmp(buff, passwd))
            break;
        vt_printf(vt, "\ninvalid password\n");
    }
    return 1;
}
/* read antenna file ---------------------------------------------------------*/
static void readant(prcopt_t *opt, nav_t *nav)
{
    const pcv_t pcv0 = {0};
    pcvs_t pcvr = {0}, pcvs = {0};
    pcv_t *pcv;
    gtime_t time = timeget();
    int i;

    trace(3, "readant:\n");

    opt->pcvr[0] = opt->pcvr[1] = pcv0;
    if (!*filopt.rcvantp)
        return;

    if (readpcv(filopt.rcvantp, &pcvr))
    {
        for (i = 0; i < 2; i++)
        {
            if (!*opt->anttype[i])
                continue;
            if (!(pcv = searchpcv(0, opt->anttype[i], time, &pcvr)))
            {
                printf("no antenna %s in %s\n", opt->anttype[i], filopt.rcvantp);
                continue;
            }
            opt->pcvr[i] = *pcv;
        }
    }
    else
        printf("antenna file open error %s\n", filopt.rcvantp);

    if (readpcv(filopt.satantp, &pcvs))
    {
        for (i = 0; i < MAXSAT; i++)
        {
            if (!(pcv = searchpcv(i + 1, "", time, &pcvs)))
                continue;
            nav->pcvs[i] = *pcv;
        }
    }
    else
        printf("antenna file open error %s\n", filopt.satantp);

    free(pcvr.pcv);
    free(pcvs.pcv);
}
/* read gps/bds navigation data from file-------------------------------------*/
static int readnavf(nav_t *nav, const char *file)
{
    gtime_t ts = {0}, te = {0};
    int stat = 0;
    /* read rinex obs and nav file */
    stat = readrnxt(file, 0, ts, te, 0.0, "", NULL, nav, NULL);
    if (stat < 0)
    {
        trace(1, "insufficient memory\n");
        return 0;
    }
    /* delete duplicated ephemeris */
    uniqnav(nav);
    return stat;
}
/* start rtk server ----------------------------------------------------------*/
static int startsvr()
{
    static sta_t sta[MAXRCV] = {{""}};
    double pos[3], npos[3];
    char s1[7][MAXRCVCMD] = {"", "", "", "", "", "", ""}, *cmds[] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    char s2[7][MAXRCVCMD] = {"", "", "", "", "", "", ""}, *cmds_periodic[] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    char *ropts[] = {"", "", "", "", "", "", ""};
    char *paths[] = {strpath[0], strpath[1], strpath[2], strpath[3],  strpath[4],  strpath[5],  strpath[6],
                     strpath[7], strpath[8], strpath[9], strpath[10], strpath[11], strpath[12], strpath[13]};
    char errmsg[2048] = "";
    int i, ret, stropt[12] = {0};

    trace(3, "startsvr:\n");
    if (prcopt.refpos == 4)
    { /* rtcm */
        for (i = 0; i < 3; i++)
            prcopt.rb[i] = 0.0;
    }
    pos[0] = nmeapos[0] * D2R;
    pos[1] = nmeapos[1] * D2R;
    pos[2] = nmeapos[2];
    pos2ecef(pos, npos);

    /* read antenna file */
    readant(&prcopt, &svr.nav);

    /* read ground truth solutions */
    read_gt_sols(filopt.gtfile, &svr.gtsols, prcopt.gtfmt);

    /* read geomagnetic field modeling cof. */
    if (svr.rtk.opt.insopt.magh)
    {
        if (!magmodel(filopt.magfile))
        {
            trace(2, "read geomagnetic field modeling cof. fail\n");
        }
    }
    /* read dcb file */
    if (filopt.dcb[0])
    {
        strcpy(sta[0].name, sta_name);
        readdcb(filopt.dcb, &svr.nav, sta);
    }
    /* read navigation data file */
    if (filopt.navfile[0])
    {
        readnavf(&svr.nav, filopt.navfile);
    }
    /* read navigation data file */
    if (filopt.bdsfile[0])
    {
        readnavf(&svr.nav, filopt.bdsfile);
    }
    uniqnav(&svr.nav);
    for (i = 0; *rcvopts[i].name; i++)
        modflgr[i] = 0;
    for (i = 0; *sysopts[i].name; i++)
        modflgs[i] = 0;

    /* set stream options */
    stropt[0] = timeout;
    stropt[1] = reconnect;
    stropt[2] = 1000;
    stropt[3] = buffsize;
    stropt[4] = fswapmargin;
    strsetopt(stropt);

    if (strfmt[2] == 8)
        strfmt[2] = STRFMT_SP3;

    /* set ftp/http directory and proxy */
    strsetdir(filopt.tempdir);
    strsetproxy(proxyaddr);

/* execute start command */
#if 0
    if (*startcmd && (ret = system(startcmd)))
    {
        trace(2, "command exec error: %s (%d)\n", startcmd, ret);
        vt_printf(vt, "command exec error: %s (%d)\n", startcmd, ret);
    }
#endif
    /* virtual console */
    svr.vt = nullptr;

    /* start rtk server */
    if (!rtksvrstart(&svr, svrcycle, buffsize, strtype, paths, strfmt, navmsgsel, cmds, cmds_periodic, ropts, nmeacycle,
                     nmeareq, npos, &prcopt, solopt, &moni, errmsg))
    {
        trace(2, "rtk server start error (%s)\n", errmsg);
        printf("rtk server start error (%s)\n", errmsg);
        return 0;
    }
    return 1;
}
/* stop rtk server -----------------------------------------------------------*/
static void stopsvr()
{
    char s[3][MAXRCVCMD] = {"", "", ""}, *cmds[] = {NULL, NULL, NULL};
    int i, ret;

    trace(3, "stopsvr:\n");

    if (!svr.state)
        return;
    /* stop rtk server */
    rtksvrstop(&svr, cmds);
}
/* open socket for remote console --------------------------------------------*/
static int open_sock(int port)
{
    struct sockaddr_in addr;
    int sock, on = 1;

    trace(3, "open_sock: port=%d\n", port);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        fprintf(stderr, "socket error (%d)\n", errno);
        return 0;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "bind error (%d)\n", errno);
        close(sock);
        return -1;
    }
    listen(sock, 5);
    return sock;
}
/* rtkrcv main -----------------------------------------------------------------
 * sysnopsis
 *     rtkrcv [-s][-p port][-d dev][-o file][-r level][-t level][-sta sta]
 *
 * description
 *     A command line version of the real-time positioning AP by rtklib. To start
 *     or stop RTK server, to configure options or to print solution/status,
 *     login a console and input commands. As default, /dev/tty is used for the
 *     console. Use -p option for network login with telnet protocol. To show
 *     the available commands, type ? or help on the console. With -p option,
 *     multiple telnet console logins are allowed. The initial processing options
 *     are loaded from default file rtkrcv.conf. To change the file, use -o
 *     option. To configure the processing options, edit the options file or use
 *     set, load or save command on the console. To shutdown the program, use
 *     shutdown command on the console or send USR2 signal to the process.
 *
 * option
 *     -s         start RTK server on program startup
 *     -p port    port number for telnet console
 *     -m port    port number for monitor stream
 *     -g port    port number for ground truth monitor stream
 *     -d dev     terminal device for console
 *     -o file    processing options file
 *     -w pwd     login password for remote console ("": no password)
 *     -r level   output solution status file (0:off,1:states,2:residuals)
 *     -t level   debug trace level (0:off,1-5:on)
 *     -sta sta   station name for receiver dcb
 *
 * command
 *     start
 *       Start RTK server. No need the command if the program runs with -s
 *       option.
 *
 *     stop
 *       Stop RTK server.
 *
 *     restart
 *       Restart RTK server. If the processing options are set, execute the
 *       command to enable the changes.
 *
 *     solution [cycle]
 *       Show solutions. Without option, only one solution is shown. With
 *       option, the soluiton is displayed at intervals of cycle (s). To stop
 *       cyclic display, send break (ctr-C).
 *
 *     status [cycle]
 *       Show RTK status. Use option cycle for cyclic display.
 *
 *     satellite [-n] [cycle]
 *       Show satellite status. Use option cycle for cyclic display. Option -n
 *       specify number of frequencies.
 *
 *     observ [-n] [cycle]
 *       Show observation data. Use option cycle for cyclic display. Option -n
 *       specify number of frequencies.
 *
 *     navidata [cycle]
 *       Show navigation data. Use option cycle for cyclic display.
 *
 *     stream [cycle]
 *       Show stream status. Use option cycle for cyclic display.
 *
 *     error
 *       Show error/warning messages. To stop messages, send break (ctr-C).
 *
 *     option [opt]
 *       Show the values of processing options. Without option, all options are
 *       displayed. With option, only pattern-matched options are displayed.
 *
 *     set opt [val]
 *       Set the value of a processing option to val. With out option val,
 *       prompt message is shown to input the value. The change of the
 *       processing option is not enabled before RTK server is restarted.
 *
 *     load [file]
 *       Load processing options from file. Without option, default file
 *       rtkrcv.conf is used. To enable the changes, restart RTK server.
 *
 *     save [file]
 *       Save current processing optons to file. Without option, default file
 *       rtkrcv.conf is used.
 *
 *     log [file|off]
 *       Record console log to file. To stop recording the log, use option off.
 *
 *     help|? [path]
 *       Show the command list. With option path, the stream path options are
 *       shown.
 *
 *     exit
 *       Exit and logout console. The status of RTK server is not affected by
 *       the command.
 *
 *     shutdown
 *       Shutdown RTK server and exit the program.
 *
 *     !command [arg...]
 *       Execute command by the operating system shell. Do not use the
 *       interactive command.
 *
 *     imudata
 *       Show imu data
 *
 *     pause
 *       Pause program. This option always use In Post-Process
 *
 *     resume
 *       Resume program. This option always use In Post-Process
 *
 * notes
 *     Short form of a command is allowed. In case of the short form, the
 *     command is distinguished according to header characters.
 *
 *-----------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
    con_t *con[MAXCON] = {0};
    int i, start = 0, port = 0, outstat = 0, trace = 0, sock = 0;
    char *dev = "", file[MAXSTR] = "";

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-s"))
            start = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            moniport = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc)
            gtmoniport = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dev = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            strcpy(file, argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            strcpy(passwd, argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            outstat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            trace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sta") && i + 1 < argc)
            strcpy(sta_name, argv[++i]);
        else
            printusage();
    }
    if (trace > 0)
    {
        traceopen(TRACEFILE);
        tracelevel(trace);
    }
    /* initialize rtk server and monitor port */
    rtksvrinit(&svr);
    strinit(&moni);

    /* initialize ground truth monitor port */
    strinit(&gtmoni);

    /* load options file */
    if (!*file)
        sprintf(file, "%s/%s", OPTSDIR, OPTSFILE);
    resetsysopts();
    if (!loadopts(file, rcvopts) || !loadopts(file, sysopts) || !loadopts(file, insopts))
    {
        fprintf(stderr, "no options file: %s. defaults used\n", file);
    }
#if 0
    getsysopts(NULL,NULL,&filopt);
    rtk_debug_opt(&prcopt,&solopt[0],NULL);
#else
    getsysopts(&prcopt, solopt, &filopt);
#endif

    /* read navigation data */
    if (!readnav(NAVIFILE, &svr.nav))
    {
        fprintf(stderr, "no navigation data: %s\n", NAVIFILE);
    }
    if (outstat > 0)
    {
        rtkopenstat(STATFILE, outstat);
    }
#if 0
    /* open ground truth monitor port */
    if (gtmoniport > 0 && !open_gtmoni(gtmoniport))
    {
        fprintf(stderr, "ground truth monitor port open error: %d\n", gtmoniport);
    }
    svr.groundtruth = &gtmoni;

    /* open monitor port */
    if (moniport > 0 && !openmoni(moniport))
    {
        fprintf(stderr, "monitor port open error: %d\n", moniport);
    }
    if (port)
    {
        /* open socket for remote console */
        if ((sock = open_sock(port)) <= 0)
        {
            fprintf(stderr, "console open error port=%d\n", port);
            if (moniport > 0)
                closemoni();
            if (outstat > 0)
                rtkclosestat();
            traceclose();
            return -1;
        }
    }
#endif
#if 0
    else {
        /* open device for local console */
        if (!(con[0]=con_open(0,dev))) {
            fprintf(stderr,"console open error dev=%s\n",dev);
            if (moniport>0) closemoni();
            if (outstat>0) rtkclosestat();
            traceclose();
            return -1;
        }
    }
#endif
    signal(SIGINT, sigshut);  /* keyboard interrupt */
    signal(SIGTERM, sigshut); /* external shutdown signal */
    signal(SIGUSR2, sigshut);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
#if OPENPLOT
    /* real-time plot */
    if (moniport)
    {
        if (moniport)
        {
            sprintf(parg, "./rtkplot_qt -p tcpcli://localhost:%d", moniport);
            execcmd(parg);
        }
    }
#endif
    /* start rtk server */
    startsvr();
    while (!intflg)
    {
        /* accept remote console connection */

#if 0
        accept_sock(sock, con);
#endif
        sleepms(100);
    }
    /* stop rtk server */
    stopsvr();
    rtksvrfree(&svr);

    /* close consoles */

#if 0
    for (i = 0; i < MAXCON; i++)
        con_close(con[i]);
#endif

    /* close monitor */
    if (gtmoniport > 0)
        closemoni_gt();
    if (moniport > 0)
        closemoni();
    if (outstat > 0)
        rtkclosestat();

    /* save navigation data */
    if (!savenav(NAVIFILE, &svr.nav))
    {
        fprintf(stderr, "navigation data save error: %s\n", NAVIFILE);
    }
    traceclose();
    return 0;
}
