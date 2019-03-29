/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2014,2015,2016,2017,2018,2019, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */

/*! \internal \file
 *
 * \brief
 * Implements Interactive Molecular Dynamics
 *
 * Re-implementation of basic IMD functions to work with VMD,
 * see imdsocket.h for references to the IMD API.
 *
 * \author Martin Hoefling, Carsten Kutzner <ckutzne@gwdg.de>
 *
 * \ingroup module_imd
 */
#include "gmxpre.h"

#include "imd.h"

#include "config.h"

#include <cerrno>
#include <cstring>

#include "gromacs/commandline/filenm.h"
#include "gromacs/domdec/domdec_struct.h"
#include "gromacs/domdec/ga2la.h"
#include "gromacs/fileio/confio.h"
#include "gromacs/fileio/gmxfio.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/gmxlib/network.h"
#include "gromacs/imd/imdsocket.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/broadcaststructs.h"
#include "gromacs/mdlib/groupcoord.h"
#include "gromacs/mdlib/sighandler.h"
#include "gromacs/mdlib/stat.h"
#include "gromacs/mdtypes/enerdata.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/mdrunoptions.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/timing/wallcycle.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/topology/topology.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/logger.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringutil.h"

/*! \brief How long shall we wait in seconds until we check for a connection again? */
constexpr int c_loopWait = 1;

/*! \brief How long shall we check for the IMD_GO? */
constexpr int c_connectWait = 1;

/*! \brief IMD Header Size. */
constexpr int c_headerSize = 8;

/*! \brief IMD Protocol Version. */
constexpr int c_protocolVersion = 2;


/*! \internal
 * \brief
 * IMD (interactive molecular dynamics) energy record.
 *
 * As in the original IMD implementation. Energies in kcal/mol.
 * NOTE: We return the energies in GROMACS / SI units,
 * so they also show up as SI in VMD.
 *
 */
typedef struct
{
    int32_t     tstep;   /**< time step                                     */
    float       T_abs;   /**< absolute temperature                          */
    float       E_tot;   /**< total energy                                  */
    float       E_pot;   /**< potential energy                              */
    float       E_vdw;   /**< van der Waals energy                          */
    float       E_coul;  /**< Coulomb interaction energy                    */
    float       E_bond;  /**< bonds energy                                  */
    float       E_angle; /**< angles energy                                 */
    float       E_dihe;  /**< dihedrals energy                              */
    float       E_impr;  /**< improper dihedrals energy                     */
} IMDEnergyBlock;


/*! \internal
 * \brief IMD (interactive molecular dynamics) communication structure.
 *
 * This structure defines the IMD communication message header & protocol version.
 */
typedef struct
{
    int32_t type;      /**< Type of IMD message, see IMDType_t above      */
    int32_t length;    /**< Length                                        */
} IMDHeader;


/*! \internal
 * \brief IMD (interactive molecular dynamics) main data structure.
 *
 * Contains private IMD data
 *
 * \todo Rename this e.g. ImdSession, and make it model
 * IForceProvider.
 */
typedef struct t_gmx_IMD
{
    bool       sessionPossible;      /**< True if tpr and mdrun input combine to permit IMD sessions */
    FILE      *outf;                 /**< Output file for IMD data, mainly forces.    */

    int        nat;                  /**< Number of atoms that can be pulled via IMD. */
    int        nat_loc;              /**< Part of the atoms that are local.           */
    int       *ind;                  /**< Global indices of the IMD atoms.            */
    int       *ind_loc;              /**< Local indices of the IMD atoms.             */
    int        nalloc_loc;           /**< Allocation size for ind_loc.                */
    rvec      *xa;                   /**< Positions for all IMD atoms assembled on
                                          the master node.                            */
    ivec      *xa_shifts;            /**< Shifts for all IMD atoms, to make
                                          molecule(s) whole.                          */
    ivec      *xa_eshifts;           /**< Extra shifts since last DD step.            */
    rvec      *xa_old;               /**< Old positions for all IMD atoms on master.  */
    int       *xa_ind;               /**< Position of each local atom in the
                                          collective array.                           */

    int             nstimd;          /**< Global IMD frequency, known to all nodes.   */
    int             nstimd_new;      /**< New frequency from IMD client, master only. */
    int             nstimd_def;      /**< Default IMD frequency when disconnected.    */

    int             port;            /**< Port to use for network socket.             */
    IMDSocket      *socket;          /**< The IMD socket on the master node.          */
    IMDSocket      *clientsocket;    /**< The IMD socket on the client.               */
    int             length;          /**< Length we got with last header.             */

    gmx_bool        bWConnect;       /**< Shall we block and wait for connection?     */
    gmx_bool        bTerminated;     /**< Set if MD is terminated.                    */
    gmx_bool        bTerminatable;   /**< Set if MD can be terminated.                */
    gmx_bool        bConnected;      /**< Set if connection is present.               */
    gmx_bool        bNewForces;      /**< Set if we received new forces.              */
    gmx_bool        bForceActivated; /**< Set if pulling from VMD is allowed.         */

    IMDEnergyBlock *energies;        /**< Pointer to energies we send back.           */

    int32_t         vmd_nforces;     /**< Number of VMD forces.                       */
    int32_t        *vmd_f_ind;       /**< VMD forces indices.                         */
    float          *vmd_forces;      /**< The VMD forces flat in memory.              */
    int             nforces;         /**< Number of actual MD forces;
                                          this gets communicated to the clients.      */
    int            *f_ind;           /**< Force indices.                              */
    rvec           *f;               /**< The IMD pulling forces.                     */

    char           *forcesendbuf;    /**< Buffer for force sending.                   */
    char           *coordsendbuf;    /**< Buffer for coordinate sending.              */
    char           *energysendbuf;   /**< Send buffer for energies.                   */
    rvec           *sendxbuf;        /**< Buffer to make molecules whole before
                                          sending.                                    */

    t_block         mols;            /**< Molecules block in IMD group.               */

    /* The next block is used on the master node only to reduce the output
     * without sacrificing information. If any of these values changes,
     * we need to write output */
    int       old_nforces;       /**< Old value for nforces.                      */
    int      *old_f_ind;         /**< Old values for force indices.               */
    rvec     *old_forces;        /**< Old values for IMD pulling forces.          */

    // TODO make this a const reference when this struct has a constructor
    const gmx::MDLogger *mdlog;     /**< Logger */

} t_gmx_IMD_setup;


/*! \internal
 * \brief Enum for types of IMD messages.
 *
 * We use the same records as the NAMD/VMD IMD implementation.
 */
typedef enum IMDType_t
{
    IMD_DISCONNECT,      /**< client disconnect                               */
    IMD_ENERGIES,        /**< energy data                                     */
    IMD_FCOORDS,         /**< atomic coordinates                              */
    IMD_GO,              /**< start command for the simulation                */
    IMD_HANDSHAKE,       /**< handshake to determine little/big endianness    */
    IMD_KILL,            /**< terminates the simulation                       */
    IMD_MDCOMM,          /**< force data                                      */
    IMD_PAUSE,           /**< pauses the simulation                           */
    IMD_TRATE,           /**< sets the IMD transmission and processing rate   */
    IMD_IOERROR,         /**< I/O error                                       */
    IMD_NR               /**< number of entries                               */
} IMDMessageType;


/*! \internal
 * \brief Names of the IMDType for error messages.
 */
static const char *eIMDType_names[IMD_NR + 1] = {
    "IMD_DISCONNECT",
    "IMD_ENERGIES",
    "IMD_FCOORDS",
    "IMD_GO",
    "IMD_HANDSHAKE",
    "IMD_KILL",
    "IMD_MDCOMM",
    "IMD_PAUSE",
    "IMD_TRATE",
    "IMD_IOERROR",
    nullptr
};



/*! \brief Fills the header with message and the length argument. */
static void fill_header(IMDHeader *header, IMDMessageType type, int32_t length)
{
    /* We (ab-)use htonl network function for the correct endianness */
    header->type   = imd_htonl(static_cast<int32_t>(type));
    header->length = imd_htonl(length);
}


/*! \brief Swaps the endianess of the header. */
static void swap_header(IMDHeader *header)
{
    /* and vice versa... */
    header->type   = imd_ntohl(header->type);
    header->length = imd_ntohl(header->length);
}


/*! \brief Reads multiple bytes from socket. */
static int32_t imd_read_multiple(IMDSocket *socket, char *datptr, int32_t toread)
{
    int32_t leftcount, countread;


    leftcount = toread;
    /* Try to read while we haven't reached toread */
    while (leftcount != 0)
    {
        if ((countread = imdsock_read(socket, datptr, leftcount)) < 0)
        {
            /* interrupted function call, try again... */
            if (errno == EINTR)
            {
                countread = 0;
            }
            /* this is an unexpected error, return what we got */
            else
            {
                return toread - leftcount;
            }

            /* nothing read, finished */
        }
        else if (countread == 0)
        {
            break;
        }
        leftcount -= countread;
        datptr    += countread;
    } /* end while */

    /* return nr of bytes read */
    return toread - leftcount;
}


/*! \brief Writes multiple bytes to socket in analogy to imd_read_multiple. */
static int32_t imd_write_multiple(IMDSocket *socket, const char *datptr, int32_t towrite)
{
    int32_t leftcount, countwritten;


    leftcount = towrite;
    while (leftcount != 0)
    {
        if ((countwritten = imdsock_write(socket, datptr, leftcount)) <= 0)
        {
            if (errno == EINTR)
            {
                countwritten = 0;
            }
            else
            {
                return towrite - leftcount;
            }
        }
        leftcount -= countwritten;
        datptr    += countwritten;
    } /* end while */

    return towrite - leftcount;
}


/*! \brief Handshake with IMD client. */
static int imd_handshake(IMDSocket *socket)
{
    IMDHeader header;


    fill_header(&header, IMD_HANDSHAKE, 1);
    header.length = c_protocolVersion; /* client wants unswapped version */

    return static_cast<int>(imd_write_multiple(socket, reinterpret_cast<char *>(&header), c_headerSize) != c_headerSize);
}


/*! \brief Send energies using the energy block and the send buffer. */
static int imd_send_energies(IMDSocket *socket, const IMDEnergyBlock *energies, char *buffer)
{
    int32_t recsize;


    recsize = c_headerSize + sizeof(IMDEnergyBlock);
    fill_header(reinterpret_cast<IMDHeader *>(buffer), IMD_ENERGIES, 1);
    memcpy(buffer + c_headerSize, energies, sizeof(IMDEnergyBlock));

    return static_cast<int>(imd_write_multiple(socket, buffer, recsize) != recsize);
}


/*! \brief Receive IMD header from socket, sets the length and returns the IMD message. */
static IMDMessageType imd_recv_header(IMDSocket *socket, int32_t *length)
{
    IMDHeader header;


    if (imd_read_multiple(socket, reinterpret_cast<char *>(&header), c_headerSize) != c_headerSize)
    {
        return IMD_IOERROR;
    }
    swap_header(&header);
    *length = header.length;

    return static_cast<IMDMessageType>(header.type);
}


/*! \brief Receive force indices and forces.
 *
 * The number of forces was previously communicated via the header.
 */
static int imd_recv_mdcomm(IMDSocket *socket, int32_t nforces, int32_t *forcendx, float *forces)
{
    int retsize, retbytes;


    /* reading indices */
    retsize  = sizeof(int32_t) * nforces;
    retbytes = imd_read_multiple(socket, reinterpret_cast<char *>(forcendx), retsize);
    if (retbytes != retsize)
    {
        return FALSE;
    }

    /* reading forces as float array */
    retsize  = 3 * sizeof(float) * nforces;
    retbytes = imd_read_multiple(socket, reinterpret_cast<char *>(forces), retsize);
    if (retbytes != retsize)
    {
        return FALSE;
    }

    return TRUE;
}

/* GROMACS specific functions for the IMD implementation */
void write_IMDgroup_to_file(gmx_bool bIMD, t_inputrec *ir, const t_state *state,
                            const gmx_mtop_t *sys, int nfile, const t_filenm fnm[])
{
    t_atoms IMDatoms;


    if (bIMD)
    {
        IMDatoms = gmx_mtop_global_atoms(sys);
        write_sto_conf_indexed(opt2fn("-imd", nfile, fnm), "IMDgroup", &IMDatoms,
                               state->x.rvec_array(), state->v.rvec_array(), ir->ePBC, state->box, ir->imd->nat, ir->imd->ind);
    }
}


void dd_make_local_IMD_atoms(const gmx_domdec_t *dd, t_gmx_IMD *IMDsetup)
{
    const gmx_ga2la_t *ga2la;

    if (IMDsetup->sessionPossible)
    {
        ga2la    = dd->ga2la;

        dd_make_local_group_indices(
                ga2la, IMDsetup->nat, IMDsetup->ind, &IMDsetup->nat_loc,
                &IMDsetup->ind_loc, &IMDsetup->nalloc_loc, IMDsetup->xa_ind);
    }
}


/*! \brief Send positions from rvec.
 *
 * We need a separate send buffer and conversion to Angstrom.
 */
static int imd_send_rvecs(IMDSocket *socket, int nat, rvec *x, char *buffer)
{
    int32_t     size;
    int         i;
    float       sendx[3];
    int         tuplesize = 3 * sizeof(float);


    /* Required size for the send buffer */
    size = c_headerSize + 3 * sizeof(float) * nat;

    /* Prepare header */
    fill_header(reinterpret_cast<IMDHeader *>(buffer), IMD_FCOORDS, static_cast<int32_t>(nat));
    for (i = 0; i < nat; i++)
    {
        sendx[0] = static_cast<float>(x[i][0]) * NM2A;
        sendx[1] = static_cast<float>(x[i][1]) * NM2A;
        sendx[2] = static_cast<float>(x[i][2]) * NM2A;
        memcpy(buffer + c_headerSize + i * tuplesize, sendx, tuplesize);
    }

    return static_cast<int>(imd_write_multiple(socket, buffer, size) != size);
}


/*! \brief Initializes the IMD private data. */
static void prepareSession(t_gmx_IMD_setup *IMDsetup,
                           const gmx::MDLogger &mdlog,
                           int imdatoms, int nstimddef, int imdport)
{
    IMDsetup->sessionPossible = true;
    IMDsetup->nat             = imdatoms;
    IMDsetup->bTerminated     = FALSE;
    IMDsetup->bTerminatable   = FALSE;
    IMDsetup->bWConnect       = FALSE;
    IMDsetup->bConnected      = FALSE;
    IMDsetup->bForceActivated = FALSE;
    IMDsetup->bNewForces      = FALSE;
    IMDsetup->bForceActivated = FALSE;
    IMDsetup->nstimd          = 1;
    IMDsetup->nstimd_new      = 1;
    IMDsetup->nstimd_def      = nstimddef;
    if (imdport < 1)
    {
        IMDsetup->port        = 0;
    }
    else
    {
        IMDsetup->port        = imdport;
    }
    IMDsetup->mdlog = &mdlog;
}


/*! \brief Prepare the socket on the MASTER. */
static void imd_prepare_master_socket(t_gmx_IMD_setup *IMDsetup)
{
    int ret;


    if (imdsock_winsockinit() == -1)
    {
        gmx_fatal(FARGS, "%s Failed to initialize winsock.\n", IMDstr);
    }

    /* The rest is identical, first create and bind a socket and set to listen then. */
    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Setting up incoming socket.", IMDstr);
    IMDsetup->socket = imdsock_create();
    if (!IMDsetup->socket)
    {
        gmx_fatal(FARGS, "%s Failed to create socket.", IMDstr);
    }

    /* Bind to port */
    ret = imdsock_bind(IMDsetup->socket, IMDsetup->port);
    if (ret)
    {
        gmx_fatal(FARGS, "%s binding socket to port %d failed with error %d.\n", IMDstr, IMDsetup->port, ret);
    }

    if (imd_sock_listen(IMDsetup->socket))
    {
        gmx_fatal(FARGS, "%s socket listen failed with error %d.\n", IMDstr, ret);
    }

    if (imdsock_getport(IMDsetup->socket, &IMDsetup->port))
    {
        gmx_fatal(FARGS, "%s Could not determine port number.\n", IMDstr);
    }

    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Listening for IMD connection on port %d.", IMDstr, IMDsetup->port);
}


/*! \brief Disconnect the client. */
static void imd_disconnect(t_gmx_IMD_setup *IMDsetup)
{
    /* Write out any buffered pulling data */
    fflush(IMDsetup->outf);

    /* we first try to shut down the clientsocket */
    imdsock_shutdown(IMDsetup->clientsocket);
    if (!imdsock_destroy(IMDsetup->clientsocket))
    {
        GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Failed to destroy socket.", IMDstr);
    }

    /* then we reset the IMD step to its default, and reset the connection boolean */
    IMDsetup->nstimd_new   = IMDsetup->nstimd_def;
    IMDsetup->clientsocket = nullptr;
    IMDsetup->bConnected   = FALSE;
}


/*! \brief Prints an error message and disconnects the client.
 *
 *  Does not terminate mdrun!
 */
static void imd_fatal(t_gmx_IMD_setup *IMDsetup,
                      const char      *msg)
{
    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s %s", IMDstr, msg);
    imd_disconnect(IMDsetup);
    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s disconnected.", IMDstr);
}


/*! \brief Check whether we got an incoming connection. */
static gmx_bool imd_tryconnect(t_gmx_IMD_setup *IMDsetup)
{
    if (imdsock_tryread(IMDsetup->socket, 0, 0) > 0)
    {
        /* yes, we got something, accept on clientsocket */
        IMDsetup->clientsocket = imdsock_accept(IMDsetup->socket);
        if (!IMDsetup->clientsocket)
        {
            GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Accepting the connection on the socket failed.", IMDstr);
            return FALSE;
        }

        /* handshake with client */
        if (imd_handshake(IMDsetup->clientsocket))
        {
            imd_fatal(IMDsetup, "Connection failed.");
            return FALSE;
        }

        GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Connection established, checking if I got IMD_GO orders.", IMDstr);

        /* Check if we get the proper "GO" command from client. */
        if (imdsock_tryread(IMDsetup->clientsocket, c_connectWait, 0) != 1 || imd_recv_header(IMDsetup->clientsocket, &(IMDsetup->length)) != IMD_GO)
        {
            imd_fatal(IMDsetup, "No IMD_GO order received. IMD connection failed.");
        }

        /* IMD connected */
        IMDsetup->bConnected = TRUE;

        return TRUE;
    }

    return FALSE;
}


/*! \brief Wrap imd_tryconnect in order to make it blocking.
 *
 * Used when the simulation should wait for an incoming connection.
 */
static void imd_blockconnect(t_gmx_IMD_setup *IMDsetup)
{
    /* do not wait for connection, when e.g. ctrl+c is pressed and we will terminate anyways. */
    if (!(static_cast<int>(gmx_get_stop_condition()) == gmx_stop_cond_none))
    {
        return;
    }

    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Will wait until I have a connection and IMD_GO orders.", IMDstr);

    /* while we have no clientsocket... 2nd part: we should still react on ctrl+c */
    while ((!IMDsetup->clientsocket) && (static_cast<int>(gmx_get_stop_condition()) == gmx_stop_cond_none))
    {
        imd_tryconnect(IMDsetup);
        imd_sleep(c_loopWait);
    }
}


/*! \brief Make sure that our array holding the forces received via IMD is large enough. */
static void imd_prepare_vmd_Forces(t_gmx_IMD_setup *IMDsetup)
{
    srenew((IMDsetup->vmd_f_ind), IMDsetup->vmd_nforces);
    srenew((IMDsetup->vmd_forces), 3*IMDsetup->vmd_nforces);
}


/*! \brief Reads forces received via IMD. */
static void imd_read_vmd_Forces(t_gmx_IMD_setup *IMDsetup)
{
    /* the length of the previously received header tells us the nr of forces we will receive */
    IMDsetup->vmd_nforces = IMDsetup->length;
    /* prepare the arrays */
    imd_prepare_vmd_Forces(IMDsetup);
    /* Now we read the forces... */
    if (!(imd_recv_mdcomm(IMDsetup->clientsocket, IMDsetup->vmd_nforces, IMDsetup->vmd_f_ind, IMDsetup->vmd_forces)))
    {
        imd_fatal(IMDsetup, "Error while reading forces from remote. Disconnecting");
    }
}


/*! \brief Prepares the MD force arrays. */
static void imd_prepare_MD_Forces(t_gmx_IMD_setup *IMDsetup)
{
    srenew((IMDsetup->f_ind), IMDsetup->nforces);
    srenew((IMDsetup->f    ), IMDsetup->nforces);
}


/*! \brief Copy IMD forces to MD forces.
 *
 * Do conversion from Cal->Joule and from
 * Angstrom -> nm and from a pointer array to arrays to 3*N array.
 */
static void imd_copyto_MD_Forces(t_gmx_IMD_setup *IMDsetup)
{
    int  i;
    real conversion = CAL2JOULE * NM2A;


    for (i = 0; i < IMDsetup->nforces; i++)
    {
        /* Copy the indices, a copy is important because we may update the incoming forces
         * whenever we receive new forces while the MD forces are only communicated upon
         * IMD communication.*/
        IMDsetup->f_ind[i] = IMDsetup->vmd_f_ind[i];

        /* Convert to rvecs and do a proper unit conversion */
        IMDsetup->f[i][0] = IMDsetup->vmd_forces[3*i    ] * conversion;
        IMDsetup->f[i][1] = IMDsetup->vmd_forces[3*i + 1] * conversion;
        IMDsetup->f[i][2] = IMDsetup->vmd_forces[3*i + 2] * conversion;
    }
}


/*! \brief Return TRUE if any of the forces or indices changed. */
static gmx_bool bForcesChanged(const t_gmx_IMD_setup *IMDsetup)
{
    int i;


    /* First, check whether the number of pulled atoms changed */
    if (IMDsetup->nforces != IMDsetup->old_nforces)
    {
        return TRUE;
    }

    /* Second, check whether any of the involved atoms changed */
    for (i = 0; i < IMDsetup->nforces; i++)
    {
        if (IMDsetup->f_ind[i] != IMDsetup->old_f_ind[i])
        {
            return TRUE;
        }
    }

    /* Third, check whether all forces are the same */
    for (i = 0; i < IMDsetup->nforces; i++)
    {
        if (IMDsetup->f[i][XX] != IMDsetup->old_forces[i][XX])
        {
            return TRUE;
        }
        if (IMDsetup->f[i][YY] != IMDsetup->old_forces[i][YY])
        {
            return TRUE;
        }
        if (IMDsetup->f[i][ZZ] != IMDsetup->old_forces[i][ZZ])
        {
            return TRUE;
        }
    }

    /* All old and new forces are identical! */
    return FALSE;
}


/*! \brief Fill the old_f_ind and old_forces arrays with the new, old values. */
static void keep_old_values(t_gmx_IMD_setup *IMDsetup)
{
    int i;


    IMDsetup->old_nforces = IMDsetup->nforces;

    for (i = 0; i < IMDsetup->nforces; i++)
    {
        IMDsetup->old_f_ind[i] = IMDsetup->f_ind[i];
        copy_rvec(IMDsetup->f[i], IMDsetup->old_forces[i]);
    }
}


/*! \brief Returns TRUE if any component of the two rvecs differs. */
static inline gmx_bool rvecs_differ(const rvec v1, const rvec v2)
{
    int i;


    for (i = 0; i < DIM; i++)
    {
        if (v1[i] != v2[i])
        {
            return TRUE;
        }
    }

    return FALSE;
}


/*! \brief Write the applied pull forces to logfile.
 *
 * Call on master only!
 */
static void output_imd_forces(t_gmx_IMD_setup *IMDsetup, double time)
{
    if (bForcesChanged(IMDsetup))
    {
        /* Write time and total number of applied IMD forces */
        fprintf(IMDsetup->outf, "%14.6e%6d", time, IMDsetup->nforces);

        /* Write out the global atom indices of the pulled atoms and the forces itself,
         * write out a force only if it has changed since the last output */
        for (int i = 0; i < IMDsetup->nforces; i++)
        {
            if (rvecs_differ(IMDsetup->f[i], IMDsetup->old_forces[i]))
            {
                fprintf(IMDsetup->outf, "%9d", IMDsetup->ind[IMDsetup->f_ind[i]] + 1);
                fprintf(IMDsetup->outf, "%12.4e%12.4e%12.4e", IMDsetup->f[i][0], IMDsetup->f[i][1], IMDsetup->f[i][2]);
            }
        }
        fprintf(IMDsetup->outf, "\n");

        keep_old_values(IMDsetup);
    }
}


/*! \brief Synchronize the nodes. */
static void imd_sync_nodes(t_gmx_IMD_setup *IMDsetup, const t_commrec *cr, double t)
{
    int              new_nforces = 0;


    /* Notify the other nodes whether we are still connected. */
    if (PAR(cr))
    {
        block_bc(cr, IMDsetup->bConnected);
    }

    /* ...if not connected, the job is done here. */
    if (!IMDsetup->bConnected)
    {
        return;
    }

    /* Let the other nodes know whether we got a new IMD synchronization frequency. */
    if (PAR(cr))
    {
        block_bc(cr, IMDsetup->nstimd_new);
    }

    /* Now we all set the (new) nstimd communication time step */
    IMDsetup->nstimd = IMDsetup->nstimd_new;

    /* We're done if we don't allow pulling at all */
    if (!(IMDsetup->bForceActivated))
    {
        return;
    }

    /* OK, let's check if we have received forces which we need to communicate
     * to the other nodes */
    if (MASTER(cr))
    {
        if (IMDsetup->bNewForces)
        {
            new_nforces = IMDsetup->vmd_nforces;
        }
        /* make the "new_forces" negative, when we did not receive new ones */
        else
        {
            new_nforces = IMDsetup->vmd_nforces * -1;
        }
    }

    /* make new_forces known to the clients */
    if (PAR(cr))
    {
        block_bc(cr, new_nforces);
    }

    /* When new_natoms < 0 then we know that these are still the same forces
     * so we don't communicate them, otherwise... */
    if (new_nforces >= 0)
    {
        /* set local VMD and nforces */
        IMDsetup->vmd_nforces = new_nforces;
        IMDsetup->nforces     = IMDsetup->vmd_nforces;

        /* now everybody knows the number of forces in f_ind, so we can prepare
         * the target arrays for indices and forces */
        imd_prepare_MD_Forces(IMDsetup);

        /* we first update the MD forces on the master by converting the VMD forces */
        if (MASTER(cr))
        {
            imd_copyto_MD_Forces(IMDsetup);
            /* We also write out forces on every update, so that we know which
             * forces are applied for every step */
            if (IMDsetup->outf)
            {
                output_imd_forces(IMDsetup, t);
            }
        }

        /* In parallel mode we communicate the to-be-applied forces to the other nodes */
        if (PAR(cr))
        {
            nblock_bc(cr, IMDsetup->nforces, IMDsetup->f_ind);
            nblock_bc(cr, IMDsetup->nforces, IMDsetup->f    );
        }

        /* done communicating the forces, reset bNewForces */
        IMDsetup->bNewForces = FALSE;
    }
}


/*! \brief Reads header from the client and decides what to do. */
static void imd_readcommand(t_gmx_IMD_setup *IMDsetup)
{
    gmx_bool       IMDpaused = FALSE;
    IMDMessageType itype;


    while (IMDsetup->clientsocket && (imdsock_tryread(IMDsetup->clientsocket, 0, 0) > 0 || IMDpaused))
    {
        itype = imd_recv_header(IMDsetup->clientsocket, &(IMDsetup->length));
        /* let's see what we got: */
        switch (itype)
        {
            /* IMD asks us to terminate the simulation, check if the user allowed this */
            case IMD_KILL:
                if (IMDsetup->bTerminatable)
                {
                    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted(" %s Terminating connection and running simulation (if supported by integrator).", IMDstr);
                    IMDsetup->bTerminated = TRUE;
                    IMDsetup->bWConnect   = FALSE;
                    gmx_set_stop_condition(gmx_stop_cond_next);
                }
                else
                {
                    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted(" %s Set -imdterm command line switch to allow mdrun termination from within IMD.", IMDstr);
                }

                break;

            /* the client doen't want to talk to us anymore */
            case IMD_DISCONNECT:
                GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted(" %s Disconnecting client.", IMDstr);
                imd_disconnect(IMDsetup);
                break;

            /* we got new forces, read them and set bNewForces flag */
            case IMD_MDCOMM:
                imd_read_vmd_Forces(IMDsetup);
                IMDsetup->bNewForces = TRUE;
                break;

            /* the client asks us to (un)pause the simulation. So we toggle the IMDpaused state */
            case IMD_PAUSE:
                if (IMDpaused)
                {
                    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted(" %s Un-pause command received.", IMDstr);
                    IMDpaused = FALSE;
                }
                else
                {
                    GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted(" %s Pause command received.", IMDstr);
                    IMDpaused = TRUE;
                }

                break;

            /* the client sets a new transfer rate, if we get 0, we reset the rate
             * to the default. VMD filters 0 however */
            case IMD_TRATE:
                IMDsetup->nstimd_new = (IMDsetup->length > 0) ? IMDsetup->length : IMDsetup->nstimd_def;
                GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted(" %s Update frequency will be set to %d.", IMDstr, IMDsetup->nstimd_new);
                break;

            /* Catch all rule for the remaining IMD types which we don't expect */
            default:
                GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted(" %s Received unexpected %s.", IMDstr, enum_name(static_cast<int>(itype), IMD_NR, eIMDType_names));
                imd_fatal(IMDsetup, "Terminating connection");
                break;
        } /* end switch */
    }     /* end while  */
}


/*! \brief Open IMD output file and write header information.
 *
 * Call on master only.
 */
static FILE *open_imd_out(const char                     *fn,
                          t_gmx_IMD_setup                *IMDsetup,
                          int                             nat_total,
                          const gmx_output_env_t         *oenv,
                          const gmx::ContinuationOptions &continuationOptions)
{
    FILE       *fp;


    /* Open log file of applied IMD forces if requested */
    if (fn && oenv)
    {
        /* If we append to an existing file, all the header information is already there */
        if (continuationOptions.appendFiles)
        {
            fp = gmx_fio_fopen(fn, "a+");
        }
        else
        {
            fp = gmx_fio_fopen(fn, "w+");
            if (IMDsetup->nat == nat_total)
            {
                fprintf(fp, "# Note that you can select an IMD index group in the .mdp file if a subset of the atoms suffices.\n");
            }

            xvgr_header(fp, "IMD Pull Forces", "Time (ps)", "# of Forces / Atom IDs / Forces (kJ/mol)", exvggtNONE, oenv);

            fprintf(fp, "# Can display and manipulate %d (of a total of %d) atoms via IMD.\n", IMDsetup->nat, nat_total);
            fprintf(fp, "# column 1    : time (ps)\n");
            fprintf(fp, "# column 2    : total number of atoms feeling an IMD pulling force at that time\n");
            fprintf(fp, "# cols. 3.-6  : global atom number of pulled atom, x-force, y-force, z-force (kJ/mol)\n");
            fprintf(fp, "# then follow : atom-ID, f[x], f[y], f[z] for more atoms in case the force on multiple atoms is changed simultaneously.\n");
            fprintf(fp, "# Note that the force on any atom is always equal to the last value for that atom-ID found in the data.\n");
            fflush(fp);
        }

        /* To reduce the output file size we remember the old values and output only
         * when something changed */
        snew(IMDsetup->old_f_ind, IMDsetup->nat);  /* One can never pull on more atoms */
        snew(IMDsetup->old_forces, IMDsetup->nat);

        return fp;
    }

    fprintf(stdout, "%s For a log of the IMD pull forces explicitly specify '-if' on the command line.\n"
            "%s (Not possible with energy minimization.)\n", IMDstr, IMDstr);

    return nullptr;
}


void IMD_finalize(t_gmx_IMD *IMDsetup)
{
    if (IMDsetup->sessionPossible && IMDsetup->outf)
    {
        gmx_fio_fclose(IMDsetup->outf);
    }
}


/*! \brief Creates the molecule start-end position array of molecules in the IMD group. */
static void init_imd_prepare_mols_in_imdgroup(t_gmx_IMD_setup *IMDsetup, const gmx_mtop_t *top_global)
{
    int      i, ii;
    t_block  lmols;
    int      nat;
    int     *ind;

    nat   = IMDsetup->nat;
    ind   = IMDsetup->ind;

    lmols.nr = 0;

    /* check whether index is sorted */
    for (i = 0; i < nat-1; i++)
    {
        if (ind[i] > ind[i+1])
        {
            gmx_fatal(FARGS, "%s IMD index is not sorted. This is currently not supported.\n", IMDstr);
        }
    }

    gmx::RangePartitioning gmols = gmx_mtop_molecules(*top_global);
    snew(lmols.index, gmols.numBlocks() + 1);
    lmols.index[0] = 0;

    for (i = 0; i < gmols.numBlocks(); i++)
    {
        auto mol   = gmols.block(i);
        int  count = 0;
        for (ii = 0; ii < nat; ii++)
        {
            if (mol.inRange(ind[ii]))
            {
                count += 1;
            }
        }
        if (count > 0)
        {
            lmols.index[lmols.nr+1] = lmols.index[lmols.nr]+count;
            lmols.nr               += 1;
        }
    }
    srenew(lmols.index, lmols.nr+1);
    lmols.nalloc_index = lmols.nr+1;
    IMDsetup->mols     = lmols;
}


/*! \brief Copied and modified from groupcoord.c shift_positions_group(). */
static void shift_positions(
        const matrix box,
        rvec         x[], /* The positions [0..nr] */
        const ivec   is,  /* The shift [0..nr] */
        int          nr)  /* The number of positions */
{
    int      i, tx, ty, tz;

    /* Loop over the group's atoms */
    if (TRICLINIC(box))
    {
        for (i = 0; i < nr; i++)
        {
            tx = is[XX];
            ty = is[YY];
            tz = is[ZZ];

            x[i][XX] = x[i][XX]-tx*box[XX][XX]-ty*box[YY][XX]-tz*box[ZZ][XX];
            x[i][YY] = x[i][YY]-ty*box[YY][YY]-tz*box[ZZ][YY];
            x[i][ZZ] = x[i][ZZ]-tz*box[ZZ][ZZ];
        }
    }
    else
    {
        for (i = 0; i < nr; i++)
        {
            tx = is[XX];
            ty = is[YY];
            tz = is[ZZ];

            x[i][XX] = x[i][XX]-tx*box[XX][XX];
            x[i][YY] = x[i][YY]-ty*box[YY][YY];
            x[i][ZZ] = x[i][ZZ]-tz*box[ZZ][ZZ];
        }
    }
}


/*! \brief Removes shifts of molecules diffused outside of the box. */
static void imd_remove_molshifts(t_gmx_IMD_setup *IMDsetup, const matrix box)
{
    int     i, ii, molsize;
    ivec    largest, smallest, shift;
    t_block mols;


    mols = IMDsetup->mols;

    /* for each molecule also present in IMD group */
    for (i = 0; i < mols.nr; i++)
    {
        /* first we determine the minimum and maximum shifts for each molecule */

        clear_ivec(largest);
        clear_ivec(smallest);
        clear_ivec(shift);

        copy_ivec(IMDsetup->xa_shifts[mols.index[i]], largest);
        copy_ivec(IMDsetup->xa_shifts[mols.index[i]], smallest);

        for (ii = mols.index[i]+1; ii < mols.index[i+1]; ii++)
        {
            if (IMDsetup->xa_shifts[ii][XX] > largest[XX])
            {
                largest[XX]  = IMDsetup->xa_shifts[ii][XX];
            }
            if (IMDsetup->xa_shifts[ii][XX] < smallest[XX])
            {
                smallest[XX] = IMDsetup->xa_shifts[ii][XX];
            }

            if (IMDsetup->xa_shifts[ii][YY] > largest[YY])
            {
                largest[YY]  = IMDsetup->xa_shifts[ii][YY];
            }
            if (IMDsetup->xa_shifts[ii][YY] < smallest[YY])
            {
                smallest[YY] = IMDsetup->xa_shifts[ii][YY];
            }

            if (IMDsetup->xa_shifts[ii][ZZ] > largest[ZZ])
            {
                largest[ZZ]  = IMDsetup->xa_shifts[ii][ZZ];
            }
            if (IMDsetup->xa_shifts[ii][ZZ] < smallest[ZZ])
            {
                smallest[ZZ] = IMDsetup->xa_shifts[ii][ZZ];
            }

        }

        /* check if we what we can subtract/add to the positions
         * to put them back into the central box */
        if (smallest[XX] > 0)
        {
            shift[XX] = smallest[XX];
        }
        if (smallest[YY] > 0)
        {
            shift[YY] = smallest[YY];
        }
        if (smallest[ZZ] > 0)
        {
            shift[ZZ] = smallest[ZZ];
        }

        if (largest[XX] < 0)
        {
            shift[XX] = largest[XX];
        }
        if (largest[YY] < 0)
        {
            shift[YY] = largest[YY];
        }
        if (largest[ZZ] < 0)
        {
            shift[ZZ] = largest[ZZ];
        }

        /* is there a shift at all? */
        if ((shift[XX]) || (shift[YY]) || (shift[ZZ]))
        {
            molsize = mols.index[i+1]-mols.index[i];
            /* shift the positions */
            shift_positions(box, &(IMDsetup->xa[mols.index[i]]), shift, molsize);
        }

    }
}


/*! \brief Initialize arrays used to assemble the positions from the other nodes. */
static void init_imd_prepare_for_x_assembly(const t_commrec *cr, const rvec x[], t_gmx_IMD_setup *IMDsetup)
{
    int i, ii;


    snew(IMDsetup->xa,         IMDsetup->nat);
    snew(IMDsetup->xa_ind,     IMDsetup->nat);
    snew(IMDsetup->xa_shifts,  IMDsetup->nat);
    snew(IMDsetup->xa_eshifts, IMDsetup->nat);
    snew(IMDsetup->xa_old,     IMDsetup->nat);

    /* Save the original (whole) set of positions such that later the
     * molecule can always be made whole again */
    if (MASTER(cr))
    {
        for (i = 0; i < IMDsetup->nat; i++)
        {
            ii = IMDsetup->ind[i];
            copy_rvec(x[ii], IMDsetup->xa_old[i]);
        }
    }

    if (!PAR(cr))
    {
        IMDsetup->nat_loc = IMDsetup->nat;
        IMDsetup->ind_loc = IMDsetup->ind;

        /* xa_ind[i] needs to be set to i for serial runs */
        for (i = 0; i < IMDsetup->nat; i++)
        {
            IMDsetup->xa_ind[i] = i;
        }
    }

    /* Communicate initial coordinates xa_old to all processes */
    if (PAR(cr))
    {
        gmx_bcast(IMDsetup->nat * sizeof(IMDsetup->xa_old[0]), IMDsetup->xa_old, cr);
    }
}


/*! \brief Check for non-working integrator / parallel options. */
static void imd_check_integrator_parallel(const t_inputrec *ir, const t_commrec *cr)
{
    if (PAR(cr))
    {
        if (((ir->eI) == eiSteep) || ((ir->eI) == eiCG) || ((ir->eI) == eiLBFGS) || ((ir->eI) == eiNM))
        {
            gmx_fatal(FARGS, "%s Energy minimization via steep, CG, lbfgs and nm in parallel is currently not supported by IMD.\n", IMDstr);
        }
    }
}

t_gmx_IMD *init_IMD(const t_inputrec        *ir,
                    const t_commrec         *cr,
                    const gmx_multisim_t    *ms,
                    const gmx_mtop_t        *top_global,
                    const gmx::MDLogger     &mdlog,
                    const rvec               x[],
                    int                      nfile,
                    const t_filenm           fnm[],
                    const gmx_output_env_t  *oenv,
                    const gmx::MdrunOptions &mdrunOptions)
{
    int              i;
    int              nat_total;
    int32_t          bufxsize;

    t_gmx_IMD_setup *IMDsetup;
    snew(IMDsetup, 1);

    /* We will allow IMD sessions only if supported by the binary and
       explicitly enabled in the .tpr file */
    if (!GMX_IMD || !ir->bIMD)
    {
        return IMDsetup;
    }

    // TODO As IMD is intended for interactivity, and the .tpr file
    // opted in for that, it is acceptable to write more terminal
    // output than in a typical simulation. However, all the GMX_LOG
    // statements below should go to both the log file and to the
    // terminal. This is probably be implemented by adding a logging
    // stream named like ImdInfo, to separate it from warning and to
    // send it to both destinations.

    int nstImd;
    if (EI_DYNAMICS(ir->eI))
    {
        nstImd = ir->nstcalcenergy;
    }
    else if (EI_ENERGY_MINIMIZATION(ir->eI))
    {
        nstImd = 1;
    }
    else
    {
        GMX_LOG(mdlog.warning).appendTextFormatted("%s Integrator '%s' is not supported for Interactive Molecular Dynamics, running normally instead", IMDstr, ei_names[ir->eI]);
        return IMDsetup;
    }
    if (isMultiSim(ms))
    {
        GMX_LOG(mdlog.warning).appendTextFormatted("%s Cannot use IMD for multiple simulations or replica exchange, running normally instead", IMDstr);
        return IMDsetup;
    }

    const auto &options = mdrunOptions.imdOptions;

    bool        createSession = false;
    /* It seems we have a .tpr file that defines an IMD group and thus allows IMD connections.
     * Check whether we can actually provide the IMD functionality for this setting: */
    if (MASTER(cr))
    {
        /* Check whether IMD was enabled by one of the command line switches: */
        if (options.wait || options.terminatable || options.pull)
        {
            GMX_LOG(mdlog.warning).appendTextFormatted("%s Enabled. This simulation will accept incoming IMD connections.", IMDstr);
            createSession = true;
        }
        else
        {
            GMX_LOG(mdlog.warning).appendTextFormatted("%s None of the -imd switches was used.\n"
                                                       "%s This run will not accept incoming IMD connections", IMDstr, IMDstr);
        }
    } /* end master only */

    /* Let the other nodes know whether we want IMD */
    if (PAR(cr))
    {
        block_bc(cr, createSession);
    }

    /*... if not we are done.*/
    if (!createSession)
    {
        return IMDsetup;
    }


    /* check if we're using a sane integrator / parallel combination */
    imd_check_integrator_parallel(ir, cr);


    /*
     *****************************************************
     * From here on we assume that IMD is turned on      *
     *****************************************************
     */

    nat_total = top_global->natoms;

    /* Initialize IMD session. If we read in a pre-IMD .tpr file, imd->nat
     * will be zero. For those cases we transfer _all_ atomic positions */
    prepareSession(IMDsetup, mdlog, ir->imd->nat > 0 ? ir->imd->nat : nat_total,
                   nstImd, options.port);

    /* We might need to open an output file for IMD forces data */
    if (MASTER(cr))
    {
        IMDsetup->outf = open_imd_out(opt2fn("-if", nfile, fnm), IMDsetup, nat_total, oenv, mdrunOptions.continuationOptions);
    }

    /* Make sure that we operate with a valid atom index array for the IMD atoms */
    if (ir->imd->nat > 0)
    {
        /* Point to the user-supplied array of atom numbers */
        IMDsetup->ind = ir->imd->ind;
    }
    else
    {
        /* Make a dummy (ind[i] = i) array of all atoms */
        snew(IMDsetup->ind, nat_total);
        for (i = 0; i < nat_total; i++)
        {
            IMDsetup->ind[i] = i;
        }
    }

    /* read environment on master and prepare socket for incoming connections */
    if (MASTER(cr))
    {
        /* we allocate memory for our IMD energy structure */
        int32_t recsize = c_headerSize + sizeof(IMDEnergyBlock);
        snew(IMDsetup->energysendbuf, recsize);

        /* Shall we wait for a connection? */
        if (options.wait)
        {
            IMDsetup->bWConnect = TRUE;
            GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Pausing simulation while no IMD connection present (-imdwait).", IMDstr);
        }

        /* Will the IMD clients be able to terminate the simulation? */
        if (options.terminatable)
        {
            IMDsetup->bTerminatable = TRUE;
            GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Allow termination of the simulation from IMD client (-imdterm).", IMDstr);
        }

        /* Is pulling from IMD client allowed? */
        if (options.pull)
        {
            IMDsetup->bForceActivated = TRUE;
            GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Pulling from IMD remote is enabled (-imdpull).", IMDstr);
        }

        /* Initialize send buffers with constant size */
        snew(IMDsetup->sendxbuf, IMDsetup->nat);
        snew(IMDsetup->energies, 1);
        bufxsize = c_headerSize + 3 * sizeof(float) * IMDsetup->nat;
        snew(IMDsetup->coordsendbuf, bufxsize);
    }

    /* do we allow interactive pulling? If so let the other nodes know. */
    if (PAR(cr))
    {
        block_bc(cr, IMDsetup->bForceActivated);
    }

    /* setup the listening socket on master process */
    if (MASTER(cr))
    {
        GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s Setting port for connection requests to %d.", IMDstr, IMDsetup->port);
        imd_prepare_master_socket(IMDsetup);
        /* Wait until we have a connection if specified before */
        if (IMDsetup->bWConnect)
        {
            imd_blockconnect(IMDsetup);
        }
        else
        {
            GMX_LOG(IMDsetup->mdlog->warning).appendTextFormatted("%s -imdwait not set, starting simulation.", IMDstr);
        }
    }
    /* Let the other nodes know whether we are connected */
    imd_sync_nodes(IMDsetup, cr, 0);

    /* Initialize arrays used to assemble the positions from the other nodes */
    init_imd_prepare_for_x_assembly(cr, x, IMDsetup);

    /* Initialize molecule blocks to make them whole later...*/
    if (MASTER(cr))
    {
        init_imd_prepare_mols_in_imdgroup(IMDsetup, top_global);
    }

    return IMDsetup;
}


gmx_bool do_IMD(t_gmx_IMD       *IMDsetup,
                int64_t          step,
                const t_commrec *cr,
                gmx_bool         bNS,
                const matrix     box,
                const rvec       x[],
                double           t,
                gmx_wallcycle   *wcycle)
{
    gmx_bool         imdstep = FALSE;


    /* IMD at all? */
    if (!IMDsetup->sessionPossible)
    {
        return FALSE;
    }

    wallcycle_start(wcycle, ewcIMD);

    /* read command from client and check if new incoming connection */
    if (MASTER(cr))
    {
        /* If not already connected, check for new connections */
        if (!IMDsetup->clientsocket)
        {
            if (IMDsetup->bWConnect)
            {
                imd_blockconnect(IMDsetup);
            }
            else
            {
                imd_tryconnect(IMDsetup);
            }
        }

        /* Let's see if we have new IMD messages for us */
        if (IMDsetup->clientsocket)
        {
            imd_readcommand(IMDsetup);
        }
    }

    /* is this an IMD communication step? */
    imdstep = do_per_step(step, IMDsetup->nstimd);

    /* OK so this is an IMD step ... */
    if (imdstep)
    {
        /* First we sync all nodes to let everybody know whether we are connected to VMD */
        imd_sync_nodes(IMDsetup, cr, t);
    }

    /* If a client is connected, we collect the positions
     * and put molecules back into the box before transfer */
    if ((imdstep && IMDsetup->bConnected)
        || bNS)            /* independent of imdstep, we communicate positions at each NS step */
    {
        /* Transfer the IMD positions to the master node. Every node contributes
         * its local positions x and stores them in the assembled xa array. */
        communicate_group_positions(cr, IMDsetup->xa, IMDsetup->xa_shifts, IMDsetup->xa_eshifts,
                                    TRUE, x, IMDsetup->nat, IMDsetup->nat_loc,
                                    IMDsetup->ind_loc, IMDsetup->xa_ind, IMDsetup->xa_old, box);

        /* If connected and master -> remove shifts */
        if ((imdstep && IMDsetup->bConnected) && MASTER(cr))
        {
            imd_remove_molshifts(IMDsetup, box);
        }
    }

    wallcycle_stop(wcycle, ewcIMD);

    return imdstep;
}


void IMD_fill_energy_record(t_gmx_IMD *IMDsetup, const gmx_enerdata_t *enerd,
                            int64_t step, gmx_bool bHaveNewEnergies)
{
    if (IMDsetup->sessionPossible)
    {
        if (IMDsetup->clientsocket)
        {
            IMDEnergyBlock *ene = IMDsetup->energies;

            ene->tstep = step;

            /* In MPI-parallel simulations the energies are not accessible a at every time step.
             * We update them if we have new values, otherwise, the energy values from the
             * last global communication step are still on display in the viewer. */
            if (bHaveNewEnergies)
            {
                ene->T_abs   = static_cast<float>(enerd->term[F_TEMP   ]);
                ene->E_pot   = static_cast<float>(enerd->term[F_EPOT   ]);
                ene->E_tot   = static_cast<float>(enerd->term[F_ETOT   ]);
                ene->E_bond  = static_cast<float>(enerd->term[F_BONDS  ]);
                ene->E_angle = static_cast<float>(enerd->term[F_ANGLES ]);
                ene->E_dihe  = static_cast<float>(enerd->term[F_PDIHS  ]);
                ene->E_impr  = static_cast<float>(enerd->term[F_IDIHS  ]);
                ene->E_vdw   = static_cast<float>(enerd->term[F_LJ     ]);
                ene->E_coul  = static_cast<float>(enerd->term[F_COUL_SR]);
            }
        }
    }
}


void IMD_send_positions(t_gmx_IMD *IMDsetup)
{
    if (IMDsetup->sessionPossible && IMDsetup->clientsocket)
    {

        if (imd_send_energies(IMDsetup->clientsocket, IMDsetup->energies, IMDsetup->energysendbuf))
        {
            imd_fatal(IMDsetup, "Error sending updated energies. Disconnecting client.");
        }

        if (imd_send_rvecs(IMDsetup->clientsocket, IMDsetup->nat, IMDsetup->xa, IMDsetup->coordsendbuf))
        {
            imd_fatal(IMDsetup, "Error sending updated positions. Disconnecting client.");
        }
    }
}


void IMD_prep_energies_send_positions(t_gmx_IMD *IMDsetup,
                                      gmx_bool bIMDstep,
                                      const gmx_enerdata_t *enerd,
                                      int64_t step, gmx_bool bHaveNewEnergies,
                                      gmx_wallcycle *wcycle)
{
    if (IMDsetup->sessionPossible)
    {
        wallcycle_start(wcycle, ewcIMD);

        /* Update time step for IMD and prepare IMD energy record if we have new energies. */
        IMD_fill_energy_record(IMDsetup, enerd, step, bHaveNewEnergies);

        if (bIMDstep)
        {
            /* Send positions and energies to VMD client via IMD */
            IMD_send_positions(IMDsetup);
        }

        wallcycle_stop(wcycle, ewcIMD);
    }
}

void IMD_apply_forces(t_gmx_IMD *IMDsetup, const t_commrec *cr, rvec *f,
                      gmx_wallcycle *wcycle)
{
    if (IMDsetup->sessionPossible)
    {
        wallcycle_start(wcycle, ewcIMD);

        /* Are forces allowed at all? If not we're done */
        if (!IMDsetup->bForceActivated)
        {
            return;
        }

        for (int i = 0; i < IMDsetup->nforces; i++)
        {
            /* j are the indices in the "System group".*/
            int j = IMDsetup->ind[IMDsetup->f_ind[i]];

            /* check if this is a local atom and find out locndx */
            const int *locndx;
            if (PAR(cr) && (locndx = cr->dd->ga2la->findHome(j)))
            {
                j = *locndx;
            }

            rvec_inc(f[j], IMDsetup->f[i]);
        }

        wallcycle_start(wcycle, ewcIMD);
    }
}
