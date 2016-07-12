/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2014,2015,2016, by the GROMACS development team, led by
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
/*! \internal \brief
 * Implements part of the alexandria program.
 * \author David van der Spoel <david.vanderspoel@icm.uu.se>
 */
#include "gmxpre.h"

#include "mymol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gromacs/commandline/filenm.h"
#include "gromacs/fileio/confio.h"
#include "gromacs/fileio/pdbio.h"
#include "gromacs/gmxpreprocess/convparm.h"
#include "gromacs/gmxpreprocess/gen_ad.h"
#include "gromacs/gmxpreprocess/gpp_atomtype.h"
#include "gromacs/gmxpreprocess/gpp_nextnb.h"
#include "gromacs/gmxpreprocess/grompp-impl.h"
#include "gromacs/gmxpreprocess/notset.h"
#include "gromacs/gmxpreprocess/pdb2top.h"
#include "gromacs/gmxpreprocess/topdirs.h"
#include "gromacs/gmxpreprocess/toputil.h"
#include "gromacs/listed-forces/bonded.h"
#include "gromacs/listed-forces/manage-threading.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/force.h"
#include "gromacs/mdlib/forcerec.h"
#include "gromacs/mdlib/gmx_omp_nthreads.h"
#include "gromacs/mdlib/mdatoms.h"
#include "gromacs/mdlib/shellfc.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/topology/atoms.h"
#include "gromacs/topology/idef.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/topology/symtab.h"
#include "gromacs/topology/topology.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringutil.h"

#include "gauss_io.h"
#include "gentop_core.h"
#include "gentop_vsite.h"
#include "gmx_simple_comm.h"
#include "molprop.h"
#include "molprop_util.h"
#include "molprop_xml.h"
#include "poldata.h"
#include "poldata_xml.h"
#include "stringutil.h"

#define STRLEN 256

namespace alexandria
{

const char *immsg(immStatus imm)
{
    static const char *msg[immNR] = {
        "Unknown status",
        "OK", "Zero Dipole", "No Quadrupole", "Charged",
        "Atom type problem", "Atom number problem", "Converting from molprop",
        "Determining bond order", "RESP Initialization",
        "Charge generation", "Requested level of theory missing",
        "QM Inconsistency (ESP dipole does not match Elec)",
        "Not in training set", "No experimental data",
        "Generating shells", "Generating bonds", "Communicating MolProp"
    };

    return msg[imm];
}

static bool is_planar(rvec xi, rvec xj, rvec xk, rvec xl, t_pbc *pbc,
                      real phi_toler)
{
    int  t1, t2, t3;
    rvec r_ij, r_kj, r_kl, m, n;
    real sign, phi;

    phi = RAD2DEG*dih_angle(xi, xj, xk, xl, pbc, r_ij, r_kj, r_kl, m, n, &sign, &t1, &t2, &t3);

    return (fabs(phi) < phi_toler);
}

static bool is_linear(rvec xi, rvec xj, rvec xk, t_pbc *pbc,
                      real th_toler)
{
    int  t1, t2;
    rvec r_ij, r_kj;
    real costh, th;

    th = fabs(RAD2DEG*bond_angle(xi, xj, xk, pbc, r_ij, r_kj, &costh, &t1, &t2));
    if ((th > th_toler) || (th < 180-th_toler))
    {
        if (NULL != debug)
        {
            fprintf(debug, "Angle is %g, th_toler is %g\n", th, th_toler);
        }
        return true;
    }
    return false;
}

void MyMol::getForceConstants(const Poldata &pd)
{
    int         n;
    double      xx, sx;
    std::string params;

    for (std::vector<PlistWrapper>::iterator pw = plist_.begin();
         (pw < plist_.end()); ++pw)
    {
        switch (pw->interactionType())
        {
            case InteractionType_BONDS:
                for (ParamIterator j = pw->beginParam(); (j < pw->endParam()); ++j)
                {
                    std::string btype1, btype2;
                    if (pd.atypeToBtype( *topology_->atoms.atomtype[j->a[0]], btype1) &&
                        pd.atypeToBtype( *topology_->atoms.atomtype[j->a[1]], btype2))
                    {
                        int    ntrain;
                        double bo = 0;
                        int    lengthUnit;

                        auto   gtbs = pd.getLastGtBonds();

                        if (-1 == (lengthUnit = string2unit(gtbs.getLengthUnit().c_str())))
                        {
                            gmx_fatal(FARGS, "Unknown length unit %s for bonds", gtbs.getLengthUnit().c_str());
                        }
                        if (!pd.searchBond(btype1, btype2, &xx, &sx,
                                           &ntrain, &bo, params))
                        {
                            j->c[0] = convert2gmx(xx, lengthUnit);
                            std::vector<std::string> ptr = gmx::splitString(params);
                            n = 0;
                            for (std::vector<std::string>::iterator pi = ptr.begin(); (pi < ptr.end()); ++pi)
                            {
                                if ((pi->length() > 0) && (n < MAXFORCEPARAM-1))
                                {
                                    j->c[1+n] = atof(pi->c_str());
                                    n++;
                                }
                            }
                        }
                    }
                    else
                    {
                        // Default bond parameters
                        j->c[0] = 0.15;
                        j->c[1] = 2e5;
                    }
                }
                break;
            case InteractionType_ANGLES:
                for (ParamIterator j = pw->beginParam(); (j < pw->endParam()); ++j)
                {
                    std::string btype1, btype2, btype3;
                    if (pd.atypeToBtype( *topology_->atoms.atomtype[j->a[0]], btype1) &&
                        pd.atypeToBtype( *topology_->atoms.atomtype[j->a[1]], btype2) &&
                        pd.atypeToBtype( *topology_->atoms.atomtype[j->a[2]], btype3))
                    {
                        int ntrain;
                        if (pd.searchAngle(btype1, btype2, btype3,
                                           &xx, &sx, &ntrain, params))
                        {
                            j->c[0] = xx;
                            std::vector<std::string> ptr = gmx::splitString(params);
                            n = 0;
                            for (std::vector<std::string>::iterator pi = ptr.begin(); (pi < ptr.end()); ++pi)
                            {
                                if ((pi->length() > 0) && (n < MAXFORCEPARAM-1))
                                {
                                    j->c[1+n] = atof(pi->c_str());
                                    n++;
                                }
                            }
                        }
                    }
                    else
                    {
                        // Default angle parameters
                        j->c[0] = 109;
                        j->c[1] = 400;
                    }
                }
                break;
            case InteractionType_PDIHS:
            case InteractionType_IDIHS:
            {
                for (ParamIterator j = pw->beginParam(); (j < pw->endParam()); ++j)
                {
                    std::string btype1, btype2, btype3, btype4;
                    if (pd.atypeToBtype( *topology_->atoms.atomtype[j->a[0]], btype1) &&
                        pd.atypeToBtype( *topology_->atoms.atomtype[j->a[1]], btype2) &&
                        pd.atypeToBtype( *topology_->atoms.atomtype[j->a[2]], btype3) &&
                        pd.atypeToBtype( *topology_->atoms.atomtype[j->a[3]], btype4))
                    {
                        int ntrain;
                        if (pd.searchDihedral(btype1, btype2, btype3, btype4,
                                              &xx, &sx, &ntrain, params))
                        {
                            j->c[0] = xx;
                            std::vector<std::string> ptr = gmx::splitString(params);
                            n = 0;
                            for (std::vector<std::string>::iterator pi = ptr.begin(); (pi < ptr.end()); ++pi)
                            {
                                if ((pi->length() > 0) && (n < MAXFORCEPARAM-1))
                                {
                                    j->c[1+n] = atof(pi->c_str());
                                    n++;
                                }
                            }
                        }
                    }
                    else if (pw->getFtype() == InteractionType_PDIHS)
                    {
                        // Default dihedral parameters
                        j->c[0] = 0;
                        j->c[1] = 5;
                        j->c[2] = 0;
                    }
                    else
                    {
                        // Default improper dihedral parameters
                        j->c[0] = 0;
                        j->c[1] = 5;
                    }
                }
            }
            break;
            case InteractionType_Polarization:
            case InteractionType_LJ14:
            case InteractionType_VSITE2:
            case InteractionType_LINEAR_ANGLES:
            case InteractionType_CONSTR:
                break;
        }
    }
}

void MyMol::MakeSpecialInteractions(bool bUseVsites)
{
    std::vector < std::vector < unsigned int> > bonds;
    std::vector<int> nbonds;
    t_pbc            pbc;
    real             th_toler = 175;
    real             ph_toler = 5;

    clear_mat(box_);
    set_pbc(&pbc, epbcNONE, box_);

    bonds.resize(topology_->atoms.nr);
    for (alexandria::BondIterator bi = molProp()->BeginBond(); (bi < molProp()->EndBond()); bi++)
    {
        // Store bonds bidirectionally to get the number correct
        bonds[bi->getAi() - 1].push_back(bi->getAj() - 1);
        bonds[bi->getAj() - 1].push_back(bi->getAi() - 1);
    }
    nbonds.resize(topology_->atoms.nr);
    for (int i = 0; (i < topology_->atoms.nr); i++)
    {
        nbonds[i] = bonds[i].size();
    }
    for (int i = 0; (i < topology_->atoms.nr); i++)
    {
        /* Now test initial geometry */
        if ((bonds[i].size() == 2) &&
            is_linear(x_[i], x_[bonds[i][0]], x_[bonds[i][1]],
                      &pbc, th_toler))
        {
            if (NULL != debug)
            {
                fprintf(debug, "found linear angle %s-%s-%s in %s\n",
                        *topology_->atoms.atomtype[bonds[i][0]],
                        *topology_->atoms.atomtype[i],
                        *topology_->atoms.atomtype[bonds[i][1]],
                        molProp()->getMolname().c_str());
            }
            gvt_.addLinear(bonds[i][0], i, bonds[i][1]);
        }
        else if ((bonds[i].size() == 3) &&
                 is_planar(x_[i], x_[bonds[i][0]],
                           x_[bonds[i][1]], x_[bonds[i][2]],
                           &pbc, ph_toler))
        {
            if (NULL != debug)
            {
                fprintf(debug, "found planar group %s-%s-%s-%s in %s\n",
                        *topology_->atoms.atomtype[i],
                        *topology_->atoms.atomtype[bonds[i][0]],
                        *topology_->atoms.atomtype[bonds[i][1]],
                        *topology_->atoms.atomtype[bonds[i][2]],
                        molProp()->getMolname().c_str());
            }
            gvt_.addPlanar(i, bonds[i][0], bonds[i][1], bonds[i][2],
                           &nbonds[0]);
        }
    }
    int anr = topology_->atoms.nr;

    gvt_.generateSpecial(bUseVsites, &topology_->atoms, &x_,
                         plist_, symtab_, atype_, &excls_);
    bHaveVSites_ = (topology_->atoms.nr > anr);
}

static void mv_plists(std::vector<PlistWrapper> &plist)
{
    for (auto &p : plist)
    {
        switch (p.interactionType())
        {
            case InteractionType_BONDS:
	         p.setFtype(F_MORSE);
                 break;
            case InteractionType_ANGLES:
	         p.setFtype(F_UREY_BRADLEY);
                 break;
            case InteractionType_LINEAR_ANGLES:
		 p.setFtype(F_LINEAR_ANGLES);
                 break;
            case InteractionType_PDIHS:
		 p.setFtype(F_PDIHS);
                 break;
            case InteractionType_IDIHS:
		 p.setFtype(F_IDIHS);
                 break;
            case InteractionType_LJ14:
            case InteractionType_Polarization:
            case InteractionType_CONSTR:
            case InteractionType_VSITE2:
                break;
        }
    }
}

static void cp_plist(t_params                  *plist,
                     int                        ftype,
                     InteractionType            itype,
                     std::vector<PlistWrapper> &plist_)
{
    if (plist->nr > 0)
    {
        PlistWrapper pw(itype, ftype);
        for (int i = 0; (i < plist->nr); i++)
        {
            // Clean up potentially not initialized values.
            for (int j = interaction_function[ftype].nratoms; j < MAXATOMLIST; j++)
            {
                plist->param[i].a[j] = 0;
            }
            for (int j = interaction_function[ftype].nrfpA; j < MAXFORCEPARAM; j++)
            {
                plist->param[i].c[j] = 0;
            }
            pw.addParam(plist->param[i]);
        }
        plist_.push_back(pw);
    }
}

void MyMol::MakeAngles(bool bPairs,
                       bool bDihs)
{
    t_nextnb                            nnb;
    t_restp                             rtp;
    t_params                            plist[F_NRE];
    std::vector<PlistWrapper>::iterator pw;

    init_plist(plist);
    for (auto &pw : plist_)
    {
        if (F_BONDS == pw.getFtype())
        {
            pr_alloc(pw.nParam(), &(plist[F_BONDS]));
            int i = 0;
            for (auto pi = pw.beginParam(); (pi < pw.endParam()); ++pi)
            {
                for (int j = 0; j < MAXATOMLIST; j++)
                {
                    plist[F_BONDS].param[i].a[j] = pi->a[j];
                }
                for (int j = 0; j < MAXFORCEPARAM; j++)
                {
                    plist[F_BONDS].param[i].c[j] = pi->c[j];
                }
                i++;
            }
            plist[F_BONDS].nr = i;
            break;
        }
    }
    /* Make Angles and Dihedrals */
    snew(excls_, topology_->atoms.nr);
    init_nnb(&nnb, topology_->atoms.nr, nexcl_+2);
    gen_nnb(&nnb, plist);

    print_nnb(&nnb, "NNB");
    rtp.bKeepAllGeneratedDihedrals    = TRUE;
    rtp.bRemoveDihedralIfWithImproper = TRUE;
    rtp.bGenerateHH14Interactions     = TRUE;
    rtp.nrexcl                        = nexcl_;
    gen_pad(&nnb, &(topology_->atoms), &rtp, plist, excls_, NULL, FALSE);

    t_blocka *EXCL;
    snew(EXCL, 1);
    generate_excl(nexcl_, topology_->atoms.nr, plist, &nnb, EXCL);
    for (int i = 0; (i < EXCL->nr); i++)
    {
        int ne = EXCL->index[i+1]-EXCL->index[i];
        srenew(excls_[i].e, ne);
        excls_[i].nr = 0;
        for (int j = EXCL->index[i]; (j < EXCL->index[i+1]); j++)
        {
            if (EXCL->a[j] != i)
            {
                excls_[i].e[excls_[i].nr++] = EXCL->a[j];
            }
        }
        // Set the rest of the memory to zero
        for (int j = excls_[i].nr; j < ne; j++)
        {
            excls_[i].e[j] = 0;
        }
    }
    done_blocka(EXCL);
    sfree(EXCL);
    if (NULL != debug)
    {
        for (int i = 0; (i < topology_->atoms.nr); i++)
        {
            fprintf(debug, "excl %d", i);
            for (int j = 0; (j < excls_[i].nr); j++)
            {
                fprintf(debug, "  %2d", excls_[i].e[j]);
            }
            fprintf(debug, "\n");
        }
    }
    done_nnb(&nnb);

    cp_plist(&plist[F_ANGLES], F_ANGLES, InteractionType_ANGLES, plist_);

    cp_plist(&plist[F_LINEAR_ANGLES], F_LINEAR_ANGLES, InteractionType_LINEAR_ANGLES, plist_);

    if (bDihs)
    {
        cp_plist(&plist[F_PDIHS], F_PDIHS, InteractionType_PDIHS, plist_);
    }

    cp_plist(&plist[F_IDIHS], F_IDIHS, InteractionType_IDIHS, plist_);

    if (bPairs)
    {
        cp_plist(&plist[F_LJ14], F_LJ14, InteractionType_LJ14, plist_);
    }

    for (int i = 0; (i < F_NRE); i++)
    {
        if (plist[i].nr > 0)
        {
            sfree(plist[i].param);
        }
    }
}

static void generate_nbparam(int ftype, int comb, double ci[], double cj[],
                             t_iparams *ip)
{
    double sig, eps;

    switch (ftype)
    {
        case F_LJ:
            switch (comb)
            {
                case eCOMB_GEOMETRIC:
                    /* Gromos rules */
                    ip->lj.c6  = sqrt(ci[0] * cj[0]);
                    ip->lj.c12 = sqrt(ci[1] * cj[1]);
                    break;

                case eCOMB_ARITHMETIC:
                    /* c0 and c1 are epsilon and sigma */
                    sig        = (ci[0]+cj[0])*0.5;
                    eps        = sqrt(ci[1]*cj[1]);
                    ip->lj.c6  = 4*eps*pow(sig, 6);
                    ip->lj.c12 = 4*eps*pow(sig, 12);

                    break;
                case eCOMB_GEOM_SIG_EPS:
                    /* c0 and c1 are epsilon and sigma */
                    sig        = sqrt(ci[0]*cj[0]);
                    eps        = sqrt(ci[1]*cj[1]);
                    ip->lj.c6  = 4*eps*pow(sig, 6);
                    ip->lj.c12 = 4*eps*pow(sig, 12);

                    break;
                default:
                    gmx_fatal(FARGS, "No such combination rule %d", comb);
            }
            break;
        default:
            gmx_fatal(FARGS, "No such function type supported %s",
                      interaction_function[ftype].name);
    }
}

static std::vector<double> getDoubles(const std::string &s)
{
    std::vector<double> d;

    for (auto &ss : gmx::splitString(s))
    {
        d.push_back(atof(ss.c_str()));
    }
    return d;
}

static void getLjParams(const Poldata     &pd,
                        const std::string &ai,
                        const std::string &aj,
                        double            *c6,
                        double            *cn)
{
    std::vector<double> vdwi, vdwj;

    auto                fai = pd.findAtype(ai);
    if (fai != pd.getAtypeEnd())
    {
        vdwi  = getDoubles(fai->getVdwparams());
    }
    else
    {
        vdwi.resize(2, 0.0);
    }
    auto faj = pd.findAtype(aj);
    if (faj != pd.getAtypeEnd())
    {
        vdwj  = getDoubles(faj->getVdwparams());
    }
    else
    {
        vdwj.resize(2, 0.0);
    }

    switch (pd.getCombRule())
    {
        case eCOMB_GEOMETRIC:
            *c6 = std::sqrt((vdwi[0]) * (vdwj[0]));
            *cn = std::sqrt((vdwi[1]) * (vdwj[1]));
            break;
        case eCOMB_ARITHMETIC:
        {
            double sig  = 0.5 * ((vdwi[0]) + (vdwj[0]));
            double eps  = std::sqrt((vdwi[1]) + (vdwj[1]));
            double sig6 = std::pow(sig, 6.0);
            *c6 = 4*eps*sig6;
            *cn = *c6 * sig6;
        }
        break;
        case eCOMB_GEOM_SIG_EPS:
        {
            double sig  = std::sqrt((vdwi[0]) * (vdwj[0]));
            double eps  = std::sqrt((vdwi[1]) * (vdwj[1]));
            double sig6 = std::pow(sig, 6.0);
            *c6 = 4*eps*sig6;
            *cn = *c6 * sig6;
        }
        break;
        case eCOMB_NONE:
        case eCOMB_NR:
            gmx_fatal(FARGS, "Unsupported combination rule for Lennard Jones");
    }
}

static void getBhamParams(const Poldata     &pd,
                          const std::string &ai,
                          const std::string &aj,
                          double            *a,
                          double            *b,
                          double            *c)
{
    std::vector<double> vdwi, vdwj;

    auto                fai = pd.findAtype(ai);
    if (fai != pd.getAtypeEnd())
    {
        vdwi  = getDoubles(fai->getVdwparams());
    }
    else
    {
        vdwi.resize(3, 0.0);
    }
    auto faj = pd.findAtype(aj);
    if (faj != pd.getAtypeEnd())
    {
        vdwj  = getDoubles(faj->getVdwparams());
    }
    else
    {
        vdwj.resize(3, 0.0);
    }

    switch (pd.getCombRule())
    {
        case eCOMB_GEOMETRIC:
            *a = std::sqrt((vdwi[0]) * (vdwj[0]));
            *b = std::sqrt((vdwi[1]) * (vdwj[1]));
            *c = std::sqrt((vdwi[2]) * (vdwj[2]));
            break;
        case eCOMB_ARITHMETIC:
            *a = 0.5 * ((vdwi[0]) + (vdwj[0]));
            *b = std::sqrt((vdwi[1]) * (vdwj[1]));
            *c = 0.5 * ((vdwi[2]) + (vdwj[2]));
            break;
        case eCOMB_KONG_MASON:
            *a = std::sqrt((vdwi[0]) * (vdwj[0]));
            *b = 2.0*(vdwi[1] * vdwj[1])/(vdwi[1] + vdwj[1]);
            *c = 0.25*((vdwi[2]/vdwi[0])+(vdwj[2]/vdwj[0]))*((vdwi[0]) + (vdwj[0]));
            break;
        case eCOMB_GEOM_SIG_EPS:
        case eCOMB_NONE:
        case eCOMB_NR:
            gmx_fatal(FARGS, "Unsupported combination rule for Buckingham");
    }
}

static void plist_to_mtop(const Poldata             &pd,
                          std::vector<PlistWrapper>  plist,
                          gmx_mtop_t                *mtop_)
{
    double fudgeLJ;
    double reppow = 12.0;
    int    n      = 0;

    /* Generate pairs */
    fudgeLJ = pd.getFudgeLJ();

    int nfptot = mtop_->ffparams.ntypes;
    for (auto &pw : plist)
    {
        nfptot += pw.nParam()*NRFPA(pw.getFtype());
    }
    srenew(mtop_->ffparams.functype, nfptot);
    srenew(mtop_->ffparams.iparams, nfptot);
    for (int i = mtop_->ffparams.ntypes; i < nfptot; i++)
    {
        mtop_->ffparams.functype[i] = 0;
        memset(&mtop_->ffparams.iparams[i], 0, sizeof(mtop_->ffparams.iparams[i]));
    }

    for (auto &pw : plist)
    {
        int ftype  = pw.getFtype();
        int nra    = NRAL(ftype);
        int nrfp   = NRFPA(ftype);
        int nratot = pw.nParam()*(1+nra);
        if (nratot > 0)
        {
            printf("There are %d interactions of type %s\n", nratot/(nra+1),
                   interaction_function[ftype].name);
        }
        snew(mtop_->moltype[0].ilist[ftype].iatoms, nratot);
        int k = 0;
        for (ParamIterator j = pw.beginParam();
             (j < pw.endParam()); ++j)
        {
            std::vector<real> c;
            c.resize(MAXFORCEPARAM, 0);
            int               l = 0;
            if (ftype == F_LJ14)
            {
                int ati = mtop_->moltype[0].atoms.atom[j->a[0]].type;
                int atj = mtop_->moltype[0].atoms.atom[j->a[1]].type;
                int tp  = ati*mtop_->ffparams.atnr+atj;
                c[l++] = mtop_->ffparams.iparams[tp].lj.c6*fudgeLJ;
                c[l++] = mtop_->ffparams.iparams[tp].lj.c12*fudgeLJ;
            }
            else
            {
                for (; (l < nrfp); l++)
                {
                    c[l] = j->c[l];
                    if (NOTSET == c[l])
                    {
                        c[l] = 0;
                    }
                }
            }
            for (; (l < MAXFORCEPARAM); l++)
            {
                c[l] = 0;
            }
            n = enter_params(&mtop_->ffparams, ftype, c.data(), 0, reppow, n, TRUE);
            mtop_->moltype[0].ilist[ftype].iatoms[k++] = n;
            for (l = 0; (l < nra); l++)
            {
                mtop_->moltype[0].ilist[ftype].iatoms[k++] = j->a[l];
            }
        }
        mtop_->moltype[0].ilist[ftype].nr = k;
    }
}

static void do_init_mtop(const Poldata            &pd,
                         gmx_mtop_t               *mtop_,
                         char                    **molname,
                         t_atoms                  *atoms,
                         std::vector<PlistWrapper> plist)
{

    init_mtop(mtop_);
    mtop_->name     = molname;
    mtop_->nmoltype = 1;
    snew(mtop_->moltype, mtop_->nmoltype);
    mtop_->moltype[0].name = molname;
    mtop_->nmolblock       = 1;
    snew(mtop_->molblock, mtop_->nmolblock);
    mtop_->molblock[0].nmol        = 1;
    mtop_->molblock[0].type        = 0;
    mtop_->molblock[0].natoms_mol  = atoms->nr;
    mtop_->natoms                  = atoms->nr;
    init_t_atoms(&(mtop_->moltype[0].atoms), atoms->nr, FALSE);

    /*Count the number of atom types in the molecule*/
    int ntype      = 0;
    for (int i = 0; (i < atoms->nr); i++)
    {
        bool found = false;
        int  itp   = atoms->atom[i].type;
        mtop_->moltype[0].atoms.atom[i] = atoms->atom[i];
        for (int j = 0; !found && (j < i); j++)
        {
            found = (itp == atoms->atom[j].type);
        }
        if (!found)
        {
            ntype++;
        }
    }

    char **atypes[ntype];
    for (int i = 0; (i < ntype); i++)
    {
        char **atp   = atoms->atomtype[i];
        bool   found = false;
        for (int j = 0; !found && (j < i); j++)
        {
            found = (atp == atoms->atomtype[j]);
        }
        if (!found)
        {
            atypes[i] = atp;
        }
    }

    mtop_->groups.grpname          = atypes;
    mtop_->groups.grps[egcENER].nr = ntype*ntype;
    mtop_->ffparams.atnr           = ntype;
    mtop_->ffparams.ntypes         = ntype*ntype;
    mtop_->ffparams.reppow         = 12;

    int vdw_type = pd.getVdwFtype();

    snew(mtop_->ffparams.functype, mtop_->ffparams.ntypes);
    snew(mtop_->ffparams.iparams, mtop_->ffparams.ntypes);
    for (int i = 0; (i < ntype); i++)
    {
        for (int j = 0; (j < ntype); j++)
        {
            int idx = ntype*i+j;
            mtop_->ffparams.functype[idx] = vdw_type;
            switch (vdw_type)
            {
                case F_LJ:
                {
                    double c6, c12;
                    getLjParams(pd,
                                *(atoms->atomtype[i]),
                                *(atoms->atomtype[j]),
                                &c6, &c12);
                    mtop_->ffparams.iparams[idx].lj.c6  = c6;
                    mtop_->ffparams.iparams[idx].lj.c12 = c12;
                }
                break;
                case F_BHAM:
                {
                    double a, b, c;
                    getBhamParams(pd,
                                  *(atoms->atomtype[i]),
                                  *(atoms->atomtype[j]),
                                  &a, &b, &c);
                    mtop_->ffparams.iparams[idx].bham.a = a;
                    mtop_->ffparams.iparams[idx].bham.b = b;
                    mtop_->ffparams.iparams[idx].bham.c = c;
                }
                break;
                default:
                    fprintf(stderr, "Invalid van der waals type %s\n",
                            pd.getVdwFunction().c_str());
            }
        }
    }

    /* Create a charge group block */
    stupid_fill_block(&(mtop_->moltype[0].cgs), atoms->nr, FALSE);

    plist_to_mtop(pd, plist, mtop_);

}

static void excls_to_blocka(int natom, t_excls excls_[], t_blocka *blocka)
{
    int i, j, k, nra;

    if (blocka->nr < natom)
    {
        srenew(blocka->index, natom+1);
        for (int i = blocka->nr; i < natom+1; i++)
        {
            blocka->index[i] = 0;
        }
    }
    nra = 0;
    for (i = 0; (i < natom); i++)
    {
        nra += excls_[i].nr;
    }
    snew(blocka->a, nra+1);
    nra = 0;
    for (i = j = 0; (i < natom); i++)
    {
        blocka->index[i] = nra;
        for (k = 0; (k < excls_[i].nr); k++)
        {
            blocka->a[j++] = excls_[i].e[k];
        }
        nra += excls_[i].nr;
    }
    blocka->index[natom] = nra;
    blocka->nr           = natom;
    blocka->nra          = nra;
}

void mtop_update_cgs(gmx_mtop_t *mtop)
{
    int i, j;

    for (i = 0; (i < mtop->nmoltype); i++)
    {
        if (mtop->moltype[i].atoms.nr > mtop->moltype[i].cgs.nr)
        {
            mtop->moltype[i].cgs.nr           = mtop->moltype[i].atoms.nr;
            mtop->moltype[i].cgs.nalloc_index = mtop->moltype[i].atoms.nr+1;
            srenew(mtop->moltype[i].cgs.index, mtop->moltype[i].cgs.nr+1);
            for (j = 0; (j <= mtop->moltype[i].cgs.nr); j++)
            {
                mtop->moltype[i].cgs.index[j] = j;
            }
        }
    }
}

bool MyMol::IsSymmetric(real toler)
{
    int       i, j, m;
    real      mm, tm;
    rvec      com, test;
    gmx_bool *bSymm, bSymmAll;

    clear_rvec(com);
    tm = 0;
    for (i = 0; (i < topology_->atoms.nr); i++)
    {
        mm  = topology_->atoms.atom[i].m;
        tm += mm;
        for (m = 0; (m < DIM); m++)
        {
            com[m] += mm*x_[i][m];
        }
    }
    if (tm > 0)
    {
        for (m = 0; (m < DIM); m++)
        {
            com[m] /= tm;
        }
    }
    for (i = 0; (i < topology_->atoms.nr); i++)
    {
        rvec_dec(x_[i], com);
    }

    snew(bSymm, topology_->atoms.nr);
    for (i = 0; (i < topology_->atoms.nr); i++)
    {
        bSymm[i] = (norm(x_[i]) < toler);
        for (j = i+1; (j < topology_->atoms.nr) && !bSymm[i]; j++)
        {
            rvec_add(x_[i], x_[j], test);
            if (norm(test) < toler)
            {
                bSymm[i] = TRUE;
                bSymm[j] = TRUE;
            }
        }
    }
    bSymmAll = TRUE;
    for (i = 0; (i < topology_->atoms.nr); i++)
    {
        bSymmAll = bSymmAll && bSymm[i];
    }
    sfree(bSymm);
    for (i = 0; (i < topology_->atoms.nr); i++)
    {
        rvec_inc(x_[i], com);
    }

    return bSymmAll;
}

static void fill_inputrec(t_inputrec *ir)
{
    ir->cutoff_scheme    = ecutsGROUP;
    ir->tabext           = 0; /* nm */
    ir->ePBC             = epbcNONE;
    ir->ns_type          = ensSIMPLE;
    ir->epsilon_r        = 1;
    ir->vdwtype          = evdwCUT;
    ir->coulombtype      = eelCUT;
    ir->coulomb_modifier = eintmodNONE;
    ir->eDispCorr        = edispcNO;
    ir->vdw_modifier     = eintmodNONE;
    ir->niter            = 25;
    ir->em_stepsize      = 1e-2; // nm
    ir->em_tol           = 1e-2;
    snew(ir->opts.egp_flags, 1);
    ir->opts.ngener = 1;
    snew(ir->fepvals, 1);
}

MyMol::MyMol() : gvt_(egvtALL)
{
    bHaveShells_       = false;
    bHaveVSites_       = false;
    cgnr_              = NULL;
    immAtoms_          = immOK;
    immTopology_       = immOK;
    immCharges_        = immOK;
    shellfc_           = nullptr;
    snew(symtab_, 1);
    open_symtab(symtab_);
    atype_ = init_atomtype();
    clear_mat(box_);
    mtop_       = nullptr;
    fr_         = nullptr;
    ltop_       = nullptr;
    mdatoms_    = nullptr;
    f_          = nullptr;
    mp_         = new MolProp;
    state_      = nullptr;
    snew(enerd_, 1);
    init_enerdata(1, 0, enerd_);

    /* Inputrec parameters */
    snew(inputrec_, 1);
    fill_inputrec(inputrec_);
}

immStatus MyMol::GenerateAtoms(gmx_atomprop_t            ap,
                               const char               *lot,
                               ChargeDistributionModel   iChargeDistributionModel)
{
    int                 myunit;
    double              xx, yy, zz;
    int                 natom;
    immStatus           imm   = immOK;

    ExperimentIterator  ci = molProp()->getLot(lot);
    if (ci < molProp()->EndExperiment())
    {
        t_param nb;

        memset(&nb, 0, sizeof(nb));
        natom = 0;
        init_t_atoms(&(topology_->atoms), ci->NAtom(), FALSE);
        snew(x_, ci->NAtom());
        snew(topology_->atoms.atomtype, ci->NAtom());
        snew(topology_->atoms.atomtypeB, ci->NAtom());

        for (CalcAtomIterator cai = ci->BeginAtom(); (cai < ci->EndAtom()); cai++)
        {
            myunit = string2unit((char *)cai->getUnit().c_str());
            if (myunit == -1)
            {
                gmx_fatal(FARGS, "Unknown length unit '%s' for atom coordinates",
                          cai->getUnit().c_str());
            }
            cai->getCoords(&xx, &yy, &zz);
            x_[natom][XX] = convert2gmx(xx, myunit);
            x_[natom][YY] = convert2gmx(yy, myunit);
            x_[natom][ZZ] = convert2gmx(zz, myunit);

            double q = 0;
            for (AtomicChargeIterator qi = cai->BeginQ(); (qi < cai->EndQ()); qi++)
            {
                // TODO Clean up this mess.
                if ((qi->getType().compare("ESP") == 0) ||
                    (name2eemtype(qi->getType()) == iChargeDistributionModel))
                {
                    myunit = string2unit((char *)qi->getUnit().c_str());
                    q      = convert2gmx(qi->getQ(), myunit);
                    break;
                }
            }
            topology_->atoms.atom[natom].q      =
                topology_->atoms.atom[natom].qB = q;

            t_atoms_set_resinfo(&(topology_->atoms), natom, symtab_, molProp()->getMolname().c_str(), 1, ' ', 1, ' ');
            topology_->atoms.atomname[natom]        = put_symtab(symtab_, cai->getName().c_str());
            topology_->atoms.atom[natom].atomnumber = gmx_atomprop_atomnumber(ap, cai->getName().c_str());

            real mass = 0;
            if (!gmx_atomprop_query(ap, epropMass, "???", cai->getName().c_str(), &mass))
            {
                fprintf(stderr, "Could not find mass for %s\n", cai->getName().c_str());
            }
            topology_->atoms.atom[natom].m      =
                topology_->atoms.atom[natom].mB = mass;

            strcpy(topology_->atoms.atom[natom].elem, gmx_atomprop_element(ap, topology_->atoms.atom[natom].atomnumber));

            topology_->atoms.atom[natom].resind = 0;
            // First set the atomtype
            topology_->atoms.atomtype[natom]      =
                topology_->atoms.atomtypeB[natom] = put_symtab(symtab_, cai->getObtype().c_str());

            natom++;
        }
        for (int i = 0; (i < natom); i++)
        {
            topology_->atoms.atom[i].type      =
                topology_->atoms.atom[i].typeB = add_atomtype(atype_, symtab_,
                                                              &(topology_->atoms.atom[i]),
                                                              *topology_->atoms.atomtype[i],
                                                              &nb,
                                                              0, 0.0, 0.0, 0.0,
                                                              topology_->atoms.atom[i].atomnumber,
                                                              0.0, 0.0);
        }
        topology_->atoms.nr   = natom;
        topology_->atoms.nres = 1;
    }
    else
    {
        imm = immLOT;
    }
    if (nullptr != debug)
    {
        fprintf(debug, "Tried to convert %s to gromacs. LOT is %s. Natoms is %d\n",
                molProp()->getMolname().c_str(), lot, natom);
    }

    return imm;
}

immStatus MyMol::checkAtoms(const Poldata &pd)
{
    int nmissing = 0;

    for (int i = 0; i < topology_->atoms.nr; i++)
    {
        const std::string    atype(*topology_->atoms.atomtype[i]);
        FfatypeConstIterator fa = pd.findAtype(atype);
        if (fa == pd.getAtypeEnd())
        {
            printf("Could not find a force field entry for atomtype %s atom %d\n",
                   *topology_->atoms.atomtype[i], i+1);
            nmissing++;
        }
    }
    if (nmissing > 0)
    {
        return immAtomTypes;
    }
    return immOK;
}

immStatus MyMol::GenerateTopology(gmx_atomprop_t          ap,
                                  const Poldata          &pd,
                                  const char             *lot,
                                  ChargeDistributionModel iChargeDistributionModel,
                                  bool                    bUseVsites,
                                  bool                    bPairs,
                                  bool                    bDih,
                                  bool                    bAddShells)
{
    immStatus imm = immOK;
    int       ftb;
    t_param   b;
    std::string btype1, btype2;

    if (nullptr != debug)
    {
        fprintf(debug, "Generating topology_ for %s\n", molProp()->getMolname().c_str());
    }

    nexcl_ = pd.getNexcl();
    molProp()->GenerateComposition(pd);
    if (molProp()->NAtom() <= 0)
    {
        imm = immAtomTypes;
    }
    if (immOK == imm)
    {
        snew(topology_, 1);
        init_top(topology_);
        /* get atoms */
        imm = GenerateAtoms(ap, lot, iChargeDistributionModel);
    }
    if (immOK == imm)
    {
        imm = checkAtoms(pd);
    }
    /* Store bonds in harmonic potential list first, update type later */
    ftb = F_BONDS;
    if (immOK == imm)
    {
        GtBondConstIterator bb;
        for (GtBondsConstIterator gtBonds = pd.getBondsBegin();
             gtBonds != pd.getBondsEnd(); gtBonds++)
        {
            int lengthUnit = string2unit(gtBonds->getLengthUnit().c_str());
            if (-1 == lengthUnit)
            {
                gmx_fatal(FARGS, "No such length unit '%s' for bonds", gtBonds->getLengthUnit().c_str());
            }
            memset(&b, 0, sizeof(b));
            for (alexandria::BondIterator bi = molProp()->BeginBond(); (bi < molProp()->EndBond()); bi++)
            {
                b.a[0] = bi->getAi() - 1;
                b.a[1] = bi->getAj() - 1;
		pd.atypeToBtype(*topology_->atoms.atomtype[b.a[0]], btype1);
		pd.atypeToBtype(*topology_->atoms.atomtype[b.a[1]], btype2);
                if (pd.findBond(btype1, btype2, 0, &bb))
                {
                    std::string         pp = bb->getParams();
                    std::vector<double> dd = getDoubles(pp);
                    int                 ii = 0;
                    b.c[ii++] = convert2gmx(bb->getLength(), lengthUnit);
                    for (auto &d : dd)
                    {
                        b.c[ii++] = d;
                    }
                    add_param_to_plist(plist_, ftb, InteractionType_BONDS, b);
                }
                else
                {
                    // Insert dummy bond to be replaced later
                    for (int i = 0; i < MAXFORCEPARAM; i++)
                    {
                        b.c[i] = 0;
                    }
                    add_param_to_plist(plist_, ftb, InteractionType_BONDS, b);
                }
            }
        }
        auto pw = SearchPlist(plist_, ftb);
        if (plist_.end() == pw || pw->nParam() == 0)
        {
            imm = immGenBonds;
        }
    }

    //printf("%s, %d plist_.size() = %d %s\n", __FILE__, __LINE__,
    //     static_cast<int>(plist_.size()), molProp()->getMolname().c_str());
    if (immOK == imm)
    {
        /* Make Angles, Dihedrals and 13 interaction. This needs the bonds to be F_BONDS. */
        MakeAngles(bPairs, bDih);

        /* Linear angles and or vsites etc. */
        //printf("%s, %d plist_.size() = %d %s\n", __FILE__, __LINE__,
        //     static_cast<int>(plist_.size()), molProp()->getMolname().c_str());
        MakeSpecialInteractions(bUseVsites);

        getForceConstants(pd);

        /* Move the plist_ to the correct function */
        mv_plists(plist_);
        //printf("%s, %d plist_.size() = %d %s\n", __FILE__, __LINE__,
        //     static_cast<int>(plist_.size()), molProp()->getMolname().c_str());

        snew(mtop_, 1);
    }
    if (bAddShells && imm == immOK)
    {
        addShells(pd, iChargeDistributionModel);
        //printf("%s, %d plist_.size() = %d %s\n", __FILE__, __LINE__,
        //     static_cast<int>(plist_.size()), molProp()->getMolname().c_str());
    }
    if (imm == immOK)
    {
        char **molnameptr = put_symtab(symtab_, molProp()->getMolname().c_str());

        do_init_mtop(pd, mtop_, molnameptr, &topology_->atoms, plist_);

        excls_to_blocka(topology_->atoms.nr, excls_,
                        &(mtop_->moltype[0].excls));
    }
    if (bAddShells && imm == immOK)
    {
        shellfc_ = init_shell_flexcon(debug, mtop_, 0, 1, false);
    }
    if (nullptr == ltop_ && imm == immOK)
    {
        ltop_ = gmx_mtop_generate_local_top(mtop_, false);
    }

    return imm;
}

void MyMol::CalcMultipoles()
{
    int                     i, m;
    rvec                    mu, mm;
    real                    r2, dfac, q;
    gmx_mtop_atomloop_all_t aloop;
    t_atom                 *atom;
    int                     at_global;

    clear_rvec(mu);
    aloop = gmx_mtop_atomloop_all_init(mtop_);
    i     = 0;
    clear_mat(Q_calc);
    clear_rvec(coq);
    while (gmx_mtop_atomloop_all_next(aloop, &at_global, &atom))
    {
        q = atom->q;
        svmul(ENM2DEBYE*q, x_[i], mm);
        rvec_inc(mu, mm);

        dfac = q*0.5*10*ENM2DEBYE;
        r2   = iprod(x_[i], x_[i]);
        for (m = 0; (m < DIM); m++)
        {
            Q_calc[m][m] += dfac*(3*gmx::square(x_[i][m]) - r2);
        }
        Q_calc[XX][YY] += dfac*3*(x_[i][XX]+coq[XX])*(x_[i][YY]+coq[YY]);
        Q_calc[XX][ZZ] += dfac*3*(x_[i][XX]+coq[XX])*(x_[i][ZZ]+coq[ZZ]);
        Q_calc[YY][ZZ] += dfac*3*(x_[i][YY]+coq[YY])*(x_[i][ZZ]+coq[ZZ]);

        i++;
    }
    GMX_RELEASE_ASSERT(i == topology_->atoms.nr, "Inconsistency 1 in mymol.cpp");
    copy_rvec(mu, mu_calc);
    dip_calc = norm(mu);
}

void MyMol::computeForces(FILE *fplog, t_commrec *cr, rvec mu_tot)
{
    tensor          force_vir;
    t_nrnb          my_nrnb;
    gmx_wallcycle_t wcycle = wallcycle_init(debug, 0, cr);
    double          t      = 0;

    init_nrnb(&my_nrnb);
    clear_mat (force_vir);
    for (int i = 0; i < mtop_->natoms; i++)
    {
        mdatoms_->chargeA[i] = mtop_->moltype[0].atoms.atom[i].q;
        if (nullptr != debug)
        {
            fprintf(debug, "QQQ Setting q[%d] to %g\n", i, mdatoms_->chargeA[i]);
        }
    }
    ltop_                = gmx_mtop_generate_local_top(mtop_, false);
    //printf("Gamma2 is %0.1f\n", ltop_->idef.iparams->bham.c);
    ltop_->idef.nthreads = 1;
    setup_bonded_threading(fr_, &ltop_->idef);

    if (nullptr != shellfc_)
    {
        relax_shell_flexcon(fplog, cr, TRUE, 0,
                            inputrec_, TRUE, ~0,
                            ltop_, nullptr, enerd_,
                            nullptr, state_,
                            f_, force_vir, mdatoms_,
                            &my_nrnb, wcycle, nullptr,
                            &(mtop_->groups),
                            shellfc_, fr_, FALSE, t, mu_tot,
                            nullptr, nullptr);
    }
    else
    {
        unsigned long flags = ~0;
        //flags = GMX_FORCE_NS | GMX_FORCE_LISTED | GMX_FORCE_NONBONDED | GMX_FORCE_FORCES | GMX_FORCE_ENERGY | GMX_FORCE_STATECHANGED;
        do_force(fplog, cr, inputrec_, 0,
                 &my_nrnb, wcycle, ltop_,
                 &(mtop_->groups),
                 box_, x_, NULL,
                 f_, force_vir, mdatoms_,
                 enerd_, NULL,
                 state_->lambda, NULL,
                 fr_,
                 NULL, mu_tot, t, NULL, NULL, FALSE,
                 flags);
    }
    for (int i = 0; i < mtop_->natoms; i++)
    {
        copy_rvec(state_->x[i], x_[i]);
    }
}

std::vector<double> MyMol::computePolarizability(double efield,
                                                 FILE *fplog, t_commrec *cr)
{

    const double        unit_factor = 29.957004; /*pol unit from (C.m**2.V*-1) to (Å**3)*/

    rvec                mu_tot;
    int                 dim;
    rvec                mu_ref;
    std::vector<double> pols;

    computeForces(fplog, cr, mu_ref);

    for (dim = 0; (dim < DIM); dim++)
    {
        inputrec_->ex[dim].a[0] = efield;
        computeForces(fplog, cr, mu_tot);
        pols.push_back(((mu_tot[dim]-mu_ref[dim])/efield)*(unit_factor));
        inputrec_->ex[dim].a[0] = 0;
    }
    return pols;
}

immStatus MyMol::GenerateCharges(const Poldata             &pd,
                                 gmx_atomprop_t             ap,
                                 ChargeDistributionModel    iChargeDistributionModel,
                                 ChargeGenerationAlgorithm  iChargeGenerationAlgorithm,
                                 real                       watoms,
                                 real                       hfac,
                                 const char                *lot,
                                 bool                       bSymmetricCharges,
                                 const char                *symm_string,
                                 t_commrec                 *cr,
                                 const char                *tabfn)
{
    rvec      mu_tot;
    immStatus imm       = immOK;
    real      tolerance = 1e-8;
    int       maxiter   = 1000;

    // This might be moved to a better place
    gmx_omp_nthreads_init(stdout, cr, 1, 1, 0, false, false);
    GenerateGromacs(cr, tabfn);
    double EspRms_ = 0;
    if (eqgESP == iChargeGenerationAlgorithm)
    {
        gr_.setChargeDistributionModel(iChargeDistributionModel);
        gr_.setAtomWeight(watoms);
    }
    if (bSymmetricCharges)
    {
        std::vector<PlistWrapper>::iterator pw = SearchPlist(plist_, InteractionType_BONDS);
        if (plist_.end() != pw)
        {
            symmetrize_charges(bSymmetricCharges,
                               &topology_->atoms,
                               pw,
                               pd, ap, symm_string, symmetric_charges_);
        }
    }

    /* Check which algorithm to use for charge generation */
    switch (iChargeGenerationAlgorithm)
    {
        case eqgNONE:
            printf("Using zero charges!\n");
            for (int i = 0; (i < topology_->atoms.nr); i++)
            {
                topology_->atoms.atom[i].q  = topology_->atoms.atom[i].qB = 0;
            }
            return immOK;
        case eqgESP:
        {
            gr_.setAtomInfo(&topology_->atoms, pd, x_);
            gr_.setAtomSymmetry(symmetric_charges_);
            gr_.setMolecularCharge(molProp()->getCharge());
            gr_.summary(debug);
            /* Even if we get the right LoT it may still not have
             * the ESP
             */
            auto ci = molProp()->getLotPropType(lot, MPO_POTENTIAL, nullptr);
            if (ci != molProp()->EndExperiment())
            {
                size_t iesp = 0;
                for (auto epi = ci->BeginPotential(); (epi < ci->EndPotential()); ++epi, ++iesp)
                {
                    if (gr_.myWeight(iesp) == 0)
                    {
                        continue;
                    }
                    int xu = string2unit(epi->getXYZunit().c_str());
                    int vu = string2unit(epi->getVunit().c_str());
                    if (-1 == xu)
                    {
                        gmx_fatal(FARGS, "No such length unit '%s' for potential",
                                  epi->getXYZunit().c_str());
                    }
                    if (-1 == vu)
                    {
                        gmx_fatal(FARGS, "No such potential unit '%s' for potential",
                                  epi->getVunit().c_str());
                    }
                    gr_.addEspPoint(convert2gmx(epi->getX(), xu),
                                    convert2gmx(epi->getY(), xu),
                                    convert2gmx(epi->getZ(), xu),
                                    convert2gmx(epi->getV(), vu));
                }
                if (debug)
                {
                    fprintf(debug, "Added %d ESP points to the RESP structure.\n",
                            static_cast<int>(gr_.nEsp()));
                }
            }

            bool   converged = false;
            double chi2[2]   = { 1e8, 1e8 };
            real   rrms      = 0, wtot;
            int    cur       = 0;
            do
            {
                gr_.updateAtomCoords(x_);
                gr_.optimizeCharges();
                for (int i = 0; i < mtop_->moltype[0].atoms.nr; i++)
                {
                    mtop_->moltype[0].atoms.atom[i].q      =
                        mtop_->moltype[0].atoms.atom[i].qB = gr_.getAtomCharge(i);
                }
                if (nullptr != shellfc_)
                {
                    computeForces(NULL, cr, mu_tot);
                }
                gr_.calcPot();
                EspRms_ = chi2[cur] = gr_.getRms(&wtot, &rrms);
                printf("RESP: RMS %g\n", chi2[cur]);
                converged = (fabs(chi2[cur] - chi2[1-cur]) < 1e-6) || (nullptr == shellfc_);
                cur       = 1-cur;
            }
            while (!converged);
            // Now copy to topology for printing
            for (int i = 0; i < topology_->atoms.nr; i++)
            {
                topology_->atoms.atom[i].q      =
                    topology_->atoms.atom[i].qB = gr_.getAtomCharge(i);
            }
        }
        break;
        case eqgEEM:
        {
            QgenEem qgen(pd, &topology_->atoms, x_,
                         iChargeDistributionModel,
                         hfac, molProp()->getCharge());

            if (eQGEN_OK != qgen.generateCharges(nullptr,
                                                 molProp()->getMolname().c_str(),
                                                 pd, &topology_->atoms,
                                                 tolerance,
                                                 maxiter))
            {
                imm = immChargeGeneration;
            }
        }
        break;
        default:
            gmx_fatal(FARGS, "Not implemented");
            break;
    }

    return imm;
}

immStatus MyMol::GenerateGromacs(t_commrec *cr, const char *tabfn)
{
    int nalloc = 2 * topology_->atoms.nr;

    if (nullptr == f_)
    {
        snew(f_, nalloc);
    }
    if (nullptr == fr_)
    {
        fr_ = mk_forcerec();
    }
    if (nullptr != tabfn)
    {
        /*Wang-Buckingham is implemented in nb-generic; thus, no need to read table now */
        //inputrec_->vdwtype   = evdwUSER;
        inputrec_->coulombtype = eelUSER;
    }
    init_forcerec(nullptr, fr_, nullptr, inputrec_, mtop_, cr,
                  box_, tabfn, tabfn, nullptr, nullptr, TRUE, -1);
    snew(state_, 1);
    init_state(state_, topology_->atoms.nr, 1, 1, 1, 0);
    mdatoms_   = init_mdatoms(nullptr, mtop_, FALSE);
    atoms2md(mtop_, inputrec_, 0, nullptr, topology_->atoms.nr, mdatoms_);
    for (int i = 0; (i < topology_->atoms.nr); i++)
    {
        copy_rvec(x_[i], state_->x[i]);
    }
    if (nullptr != shellfc_)
    {
        make_local_shells(cr, mdatoms_, shellfc_);
    }
    return immOK;
}

static void put_in_box(int natom, matrix box, rvec x[], real dbox)
{
    int  i, m;
    rvec xmin, xmax, xcom;

    clear_rvec(xcom);
    copy_rvec(x[0], xmin);
    copy_rvec(x[0], xmax);
    for (i = 0; (i < natom); i++)
    {
        rvec_inc(xcom, x[i]);
        for (m = 0; (m < DIM); m++)
        {
            if (xmin[m] > x[i][m])
            {
                xmin[m] = x[i][m];
            }
            else if (xmax[m] < x[i][m])
            {
                xmax[m] = x[i][m];
            }
        }
    }
    for (m = 0; (m < DIM); m++)
    {
        xcom[m]  /= natom;
        box[m][m] = (dbox+xmax[m]-xmin[m]);
    }
}

void MyMol::PrintConformation(const char *fn)
{
    char title[STRLEN];

    put_in_box(topology_->atoms.nr, box_, x_, 0.3);
    sprintf(title, "%s processed by alexandria", molProp()->getMolname().c_str());
    write_sto_conf(fn, title, &topology_->atoms, x_, nullptr, epbcNONE, box_);
}

static void write_zeta_q(FILE *fp, QgenEem * qgen,
                         t_atoms *atoms, ChargeDistributionModel iChargeDistributionModel)
{
    int    i, ii, j, k, nz, row;
    double zeta, q;
    bool   bAtom, bTypeSet;

    if (nullptr == qgen)
    {
        return;
    }

    fprintf(fp, "[ charge_spreading ]\n");
    fprintf(fp, "; This section describes additional atom type properties.\n");
    fprintf(fp, "; Spreading type (stype) can be either Gaussian (AXg) or Slater (AXs).\n");
    fprintf(fp, "; The zeta are the same for atoms of the same type, and all but the last\n");
    fprintf(fp, "; charge as well. The final charge is different between atoms however,\n");
    fprintf(fp, "; and it is listed below in the [ atoms ] section.\n");
    fprintf(fp, "; atype stype  nq%s      zeta          q  ...\n",
            (iChargeDistributionModel == eqdAXs) ? "  row" : "");

    k = -1;
    for (i = 0; (i < atoms->nr); i++)
    {
        bAtom = (atoms->atom[i].ptype == eptAtom);
        if (bAtom)
        {
            k++;
        }
        if (k == -1)
        {
            gmx_fatal(FARGS, "The first atom must be a real atom, not a shell");
        }
        nz = qgen->getNzeta( k);
        if (nz != NOTSET)
        {
            bTypeSet = false;
            for (ii = 0; !bTypeSet && (ii < i); ii++)
            {
                bTypeSet = (atoms->atom[ii].type == atoms->atom[i].type);
            }
            if (!bTypeSet)
            {
                fprintf(fp, "%5s %6s %3d",
                        *atoms->atomtype[i],
                        getEemtypeName(iChargeDistributionModel), (bAtom) ? nz : 1);
            }
            for (j = (bAtom ? 0 : nz); (j < (bAtom ? nz : nz)); j++)
            {
                row   = qgen->getRow( k, j);
                q     = qgen->getQ( k, j);
                zeta  = qgen->getZeta( k, j);
                if ((row != NOTSET) && (q != NOTSET) && (zeta != NOTSET))
                {
                    if (j == nz-1)
                    {
                        atoms->atom[i].q      =
                            atoms->atom[i].qB = q;
                    }
                    if (!bTypeSet)
                    {
                        if (iChargeDistributionModel == eqdAXs)
                        {
                            fprintf(fp, "  %4d", row);
                        }
                        fprintf(fp, " %10f", zeta);
                        if (j < nz-1)
                        {
                            fprintf(fp, " %10f", q);
                        }
                    }
                }
            }
            if (!bTypeSet)
            {
                fprintf(fp, "\n");
            }
        }
    }
    fprintf(fp, "\n");
}

static void write_zeta_q2(QgenEem * qgen, gpp_atomtype_t atype,
                          t_atoms *atoms, ChargeDistributionModel iChargeDistributionModel)
{
    FILE      *fp;
    int        i, j, k, nz, row;
    double     zeta, q, qtot;
    gmx_bool   bAtom;

    if (nullptr == qgen)
    {
        return;
    }

    fp = fopen("zeta_q.txt", "w");
    k  = -1;
    for (i = 0; (i < atoms->nr); i++)
    {
        bAtom = (atoms->atom[i].ptype == eptAtom);
        if (bAtom)
        {
            k++;
        }
        if (k == -1)
        {
            gmx_fatal(FARGS, "The first atom must be a real atom, not a shell");
        }
        nz = qgen->getNzeta( k);
        if (nz != NOTSET)
        {
            fprintf(fp, "%6s  %5s  %5d", getEemtypeName(iChargeDistributionModel),
                    get_atomtype_name(atoms->atom[i].type, atype),
                    (bAtom) ? nz-1 : 1);
            qtot = 0;
            for (j = (bAtom ? 0 : nz-1); (j < (bAtom ? nz-1 : nz)); j++)
            {
                row   = qgen->getRow( k, j);
                q     = qgen->getQ( k, j);
                zeta  = qgen->getZeta( k, j);
                if ((row != NOTSET) && (q != NOTSET) && (zeta != NOTSET))
                {
                    qtot += q;
                    fprintf(fp, "%5d %10g %10g", row, zeta, q);
                }
            }
            atoms->atom[i].q = qtot;
            fprintf(fp, "\n");
        }
    }
    fprintf(fp, "\n");
    fclose(fp);
}

static int get_subtype(directive d, int ftype)
{
    int i;
    for (i = 1; (i < 20); i++)
    {
        if (ifunc_index(d, i) == ftype)
        {
            return i;
        }
    }
    return 1;
}

static void print_bondeds2(FILE                     *out,
                           directive                 d,
                           int                       plist_ftype,
                           int                       print_ftype,
                           std::vector<PlistWrapper> plist)
{
    std::vector<PlistWrapper>::iterator p = SearchPlist(plist, plist_ftype);

    if (plist.end() == p || p->nParam() == 0)
    {
        return;
    }
    fprintf(out, "[ %s ]\n", dir2str(d));
    fprintf(out, ";atom i");
    for (int j = 1; (j < NRAL(print_ftype)); j++)
    {
        fprintf(out, "  %5c", j+'i');
    }
    fprintf(out, "   type  parameters\n");
    int subtype = get_subtype(d, print_ftype);
    for (ParamIterator i = p->beginParam(); (i < p->endParam()); ++i)
    {
        for (int j = 0; (j < NRAL(print_ftype)); j++)
        {
            fprintf(out, "  %5d", 1+i->a[j]);
        }
        fprintf(out, "  %5d", subtype);
        for (int j = 0; (j < NRFPA(print_ftype)); j++)
        {
            fprintf(out, "  %10g", i->c[j]);
        }
        fprintf(out, "\n");
    }
    fprintf(out, "\n");
}

static void write_top2(FILE *out, char *molname,
                       t_atoms *at, gmx_bool bRTPresname,
                       std::vector<PlistWrapper> plist_,
                       t_excls excls[],
                       gpp_atomtype_t atype, int *cgnr, int nrexcl,
                       const Poldata &pd)
/* NOTE: nrexcl is not the size of *excl! */
{
    if (at && atype && cgnr)
    {
        fprintf(out, "[ %s ]\n", dir2str(d_moleculetype));
        fprintf(out, "; %-15s %5s\n", "Name", "nrexcl");
        fprintf(out, "%-15s %5d\n\n", molname ? molname : "Protein", nrexcl);

        print_atoms(out, atype, at, cgnr, bRTPresname);

        for (GtBondsConstIterator gtbs = pd.getBondsBegin();
             gtbs != pd.getBondsEnd(); gtbs++)
        {
            print_bondeds2(out, d_bonds,
                           gtbs->getBondFtype(),
                           gtbs->getBondFtype(),
                           plist_);
        }

        print_bondeds2(out, d_constraints, F_CONSTR, F_CONSTR, plist_);
        print_bondeds2(out, d_constraints, F_CONSTRNC, F_CONSTRNC, plist_);
        print_bondeds2(out, d_pairs, F_LJ14, F_LJ14, plist_);
        print_excl(out, at->nr, excls);

        for (GtAnglesConstIterator gtas = pd.getAnglesBegin();
             gtas != pd.getAnglesEnd(); gtas++)
        {
            print_bondeds2(out, d_angles,
                           gtas->getAngleFtype(),
                           gtas->getAngleFtype(),
                           plist_);
        }

        for (GtDihedralsConstIterator gtds = pd.getDihedralsBegin();
             gtds != pd.getDihedralsEnd(); gtds++)
        {
            print_bondeds2(out, d_dihedrals,
                           gtds->getDihedralFtype(),
                           gtds->getDihedralFtype(),
                           plist_);
        }

        print_bondeds2(out, d_cmap, F_CMAP, F_CMAP, plist_);
        print_bondeds2(out, d_polarization, F_POLARIZATION, F_POLARIZATION,
                       plist_);
        print_bondeds2(out, d_thole_polarization, F_THOLE_POL, F_THOLE_POL, plist_);
        print_bondeds2(out, d_vsites2, F_VSITE2, F_VSITE2, plist_);
        print_bondeds2(out, d_vsites3, F_VSITE3, F_VSITE3, plist_);
        print_bondeds2(out, d_vsites3, F_VSITE3FD, F_VSITE3FD, plist_);
        print_bondeds2(out, d_vsites3, F_VSITE3FAD, F_VSITE3FAD, plist_);
        print_bondeds2(out, d_vsites3, F_VSITE3OUT, F_VSITE3OUT, plist_);
        print_bondeds2(out, d_vsites4, F_VSITE4FD, F_VSITE4FD, plist_);
        print_bondeds2(out, d_vsites4, F_VSITE4FDN, F_VSITE4FDN, plist_);
    }
}


static void print_top_header2(FILE *fp, const Poldata &pd,
                              gmx_atomprop_t aps, bool bPol,
                              std::vector<std::string> commercials,
                              bool bItp)
{
    std::string   gt_old, gt_type;
    std::string   btype;
    int           atomnumber;
    real          mass;

    fprintf(fp, ";\n");
    fprintf(fp, "; Topology generated by alexandria gentop.\n");
    fprintf(fp, "; Watch this space for information & commercials.\n");
    for (std::vector<std::string>::iterator i = commercials.begin(); (i < commercials.end()); ++i)
    {
        fprintf(fp, "; %s\n", i->c_str());
    }
    fprintf(fp, ";\n");
    if (!bItp)
    {
        fprintf(fp, "[ defaults ]\n");
        fprintf(fp, "; nbfunc         comb-rule       gen-pairs       fudgeLJ     fudgeQQ\n");
        std::string ff = pd.getVdwFunction();
        if (strcasecmp(ff.c_str(), "LJ_SR") == 0)
        {
            ff = "LJ";
        }
        fprintf(fp, "%-15s  %-15s no           %10g  %10g\n\n",
                ff.c_str(),
                pd.getCombinationRule().c_str(),
                pd.getFudgeLJ(),
                pd.getFudgeQQ());

        fprintf(fp, "[ atomtypes ]\n");
        fprintf(fp, "%-7s%-6s  %6s  %11s  %10s  %5s %-s  %s\n",
                ";atype ", "btype", "at.num", "mass", "charge", "ptype",
                "Van_der_Waals", "Ref_Enthalpy");

        gt_old = "";

        for (FfatypeConstIterator aType = pd.getAtypeBegin();
             aType != pd.getAtypeEnd(); aType++)
        {
            gt_type = aType->getType();
            btype   = aType->getBtype();
            if (gmx_atomprop_query(aps, epropMass, "", aType->getElem().c_str(), &mass))
            {
                atomnumber = gmx_atomprop_atomnumber(aps, aType->getElem().c_str());
                if ((0 ==  gt_old.size()) || (gt_old.compare(gt_type) != 0))
                {
                    char sgt_type[32];
                    snprintf(sgt_type, 32, "%s_s", gt_type.c_str());
                    if (0 == btype.size())
                    {
                        btype = gt_type;
                    }
                    fprintf(fp, "%-6s %-6s %6d  %12.6f  %10.4f  A     %-s  %s\n",
                            gt_type.c_str(), aType->getBtype().c_str(), atomnumber, mass, 0.0, aType->getVdwparams().c_str(),
                            aType->getRefEnthalpy().c_str());
                    if (bPol)
                    {
                        fprintf(fp, "%-6s %-6s %6d  %12.6f  %10.4f  S     0  0\n",
                                sgt_type, sgt_type, 0, 0.0, 0.0);
                    }
                }
            }
            gt_old = gt_type;
        }
        fprintf(fp, "\n");
    }
}

void MyMol::PrintTopology(const char             *fn,
                          ChargeDistributionModel iChargeDistributionModel,
                          bool                    bVerbose,
                          const Poldata          &pd,
                          gmx_atomprop_t          aps)
{
    FILE                    *fp;
    t_mols                   printmol;
    bool                     bITP;
    std::vector<std::string> commercials;
    char                     buf[256];

    CalcQPol(pd);

    if (molProp()->getMolname().size() > 0)
    {
        printmol.name = strdup(molProp()->getMolname().c_str());
    }
    else if (molProp()->formula().size() > 0)
    {
        printmol.name = strdup(molProp()->formula().c_str());
    }
    else
    {
        printmol.name = strdup("Onbekend");
    }
    printmol.nr   = 1;

    /* Write topology_ file */
    bITP = (fn2ftp(fn) == efITP);
    fp   = gmx_ffopen(fn, "w");

    snprintf(buf, sizeof(buf), "ref_enthalpy   = %.3f kJ/mol", ref_enthalpy_);
    commercials.push_back(buf);
    snprintf(buf, sizeof(buf), "polarizability = %.3f +/- %.3f A^3",
             polarizability_, sig_pol_);
    commercials.push_back(buf);
    snprintf(buf, sizeof(buf), "total charge   = %d e", molProp()->getCharge());
    commercials.push_back(buf);
    snprintf(buf, sizeof(buf), "total mass     = %.3f Da", molProp()->getMass());
    commercials.push_back(buf);
    snprintf(buf, sizeof(buf), "total dipole   = %.3f D", mutot_);
    commercials.push_back(buf);
    print_top_header2(fp, pd, aps, bHaveShells_, commercials, bITP);

    if (bHaveShells_ || (iChargeDistributionModel == eqdAXg) || (iChargeDistributionModel == eqdAXs))
    {
        //write_zeta_q(fp, qgen_, &topology_->atoms, iChargeDistributionModel);
        //write_zeta_q2(qgen,atype,&topology_->atoms,pd,iChargeDistributionModel);
    }
    write_top2(fp, printmol.name, &topology_->atoms, FALSE,
               plist_, excls_, atype_, cgnr_, nexcl_, pd);
    if (!bITP)
    {
        print_top_mols(fp, printmol.name, getForceField().c_str(), nullptr, 0, nullptr, 1, &printmol);
    }

    if (bVerbose)
    {
        for (auto &p : plist_)
        {
            if (p.nParam() > 0)
            {
                printf("There are %4d %s interactions\n", p.nParam(),
                       interaction_function[p.getFtype()].name);
            }
        }
        for (std::vector<std::string>::iterator i = commercials.begin(); (i < commercials.end()); ++i)
        {
            printf("%s\n", i->c_str());
        }
    }

    fclose(fp);

}

static void add_excl(t_excls *excls, int e)
{
    int i;

    for (i = 0; (i < excls->nr); i++)
    {
        if (excls->e[i] == e)
        {
            return;
        }
    }
    srenew(excls->e, excls->nr+1);
    excls->e[excls->nr++] = e;
}

static void add_excl_pair(t_excls excls[], int e1, int e2)
{
    if (e1 != e2)
    {
        add_excl(&excls[e1], e2);
        add_excl(&excls[e2], e1);
    }
}

static void remove_excl(t_excls *excls, int remove)
{
    int i;

    for (i = remove+1; i < excls->nr; i++)
    {
        excls->e[i-1] = excls->e[i];
    }

    excls->nr--;
}

static void prune_excl(t_excls excls[], t_atoms *atoms, gpp_atomtype_t atype)
{
    int i, k, ak;

    for (i = 0; (i < atoms->nr); i++)
    {
        if (get_atomtype_ptype(atoms->atom[i].type, atype) != eptShell)
        {
            for (k = 0; (k < excls[i].nr); )
            {
                ak = excls[i].e[k];
                if (get_atomtype_ptype(atoms->atom[ak].type, atype) != eptShell)
                {
                    remove_excl(&(excls[i]), k);
                }
                else
                {
                    k++;
                }
            }
        }
    }
}

static void copy_atoms(t_atoms *src, t_atoms *dest)
{
    int i;

    if (dest->nr < src->nr)
    {
        srenew(dest->atom, src->nr);
        srenew(dest->atomname, src->nr);
        if (nullptr != src->atomtype)
        {
            srenew(dest->atomtype, src->nr);
        }
        else if (nullptr != dest->atomtype)
        {
            sfree(dest->atomtype);
            dest->atomtype = nullptr;
        }
        if (nullptr != src->atomtypeB)
        {
            srenew(dest->atomtypeB, src->nr);
        }
        else if (nullptr != dest->atomtypeB)
        {
            sfree(dest->atomtypeB);
            dest->atomtypeB = nullptr;
        }
    }
    dest->nr = src->nr;
    for (i = 0; (i < src->nr); i++)
    {
        dest->atom[i]      = src->atom[i];
        dest->atomname[i]  = src->atomname[i];
        if (nullptr != src->atomtype)
        {
            dest->atomtype[i]  = src->atomtype[i];
        }
        if (nullptr != src->atomtypeB)
        {
            dest->atomtypeB[i] = src->atomtypeB[i];
        }
    }
    if (dest->nres < src->nres)
    {
        srenew(dest->resinfo, src->nres);
    }

    if (nullptr != src->pdbinfo)
    {
        srenew(dest->pdbinfo, src->nres);
    }
    else if (nullptr != dest->pdbinfo)
    {
        sfree(dest->pdbinfo);
        dest->pdbinfo = nullptr;
    }
    dest->nres = src->nres;
    for (i = 0; (i < src->nres); i++)
    {
        dest->resinfo[i] = src->resinfo[i];
        if (nullptr != src->pdbinfo)
        {
            dest->pdbinfo[i] = src->pdbinfo[i];
        }
    }
}

void MyMol::addShells(const Poldata          &pd,
                      ChargeDistributionModel iModel)
{
    int              i, j, k, iat, shell, nshell = 0;
    std::vector<int> renum, inv_renum;
    char             buf[32], **newname;
    t_param          p;
    t_atoms         *newa;
    t_excls         *newexcls;
    rvec            *newx;
    double           pol, sigpol;

    int              maxatom = topology_->atoms.nr*2+2;
    srenew(x_, maxatom);
    memset(&p, 0, sizeof(p));
    inv_renum.resize(topology_->atoms.nr*2, -1);
    int polarUnit = string2unit(pd.getPolarUnit().c_str());
    if (-1 == polarUnit)
    {
        gmx_fatal(FARGS, "No such polarizability unit '%s'",
                  pd.getPolarUnit().c_str());
    }
    for (i = 0; (i < topology_->atoms.nr); i++)
    {
        renum.push_back(i+nshell);
        inv_renum[i+nshell] = i;
        if (pd.getAtypePol(*topology_->atoms.atomtype[i],
                           &pol, &sigpol) &&
            (pol > 0) &&
            (pd.getNzeta(iModel, *topology_->atoms.atomtype[i]) == 2))
        {
            nshell++;
            p.a[0] = renum[i];
            p.a[1] = renum[i]+1;
            p.c[0] = convert2gmx(pol, polarUnit);
            add_param_to_plist(plist_, F_POLARIZATION, InteractionType_Polarization, p);
        }
    }
    renum.resize(1+topology_->atoms.nr, 0);
    renum[topology_->atoms.nr] = topology_->atoms.nr + nshell;
    if (NULL != debug)
    {
        fprintf(debug, "added %d shells\n", nshell);
    }
    if (nshell > 0)
    {
        t_atom          *shell_atom;
        snew(shell_atom, 1);
        shell_atom->ptype = eptShell;

        /* Make new atoms and x arrays */
        snew(newa, 1);
        init_t_atoms(newa, topology_->atoms.nr+nshell, TRUE);
        snew(newa->atomtype, topology_->atoms.nr+nshell);
        snew(newa->atomtypeB, topology_->atoms.nr+nshell);
        newa->nres = topology_->atoms.nres;
        snew(newx, newa->nr);
        snew(newname, newa->nr);

        /* Make new exclusion array, and put the shells in it */
        snew(newexcls, newa->nr);
        /* TODO: other polarization types */
        auto pw = SearchPlist(plist_, F_POLARIZATION);
        if (plist_.end() != pw)
        {
            for (ParamIterator j = pw->beginParam();
                 (j < pw->endParam()); ++j)
            {
                // Exclude nucleus and shell from each other
                add_excl_pair(newexcls, j->a[0], j->a[1]);
            }
            for (ParamIterator j = pw->beginParam();
                 (j < pw->endParam()); ++j)
            {
                // Now add the exclusions from the nucleus to the shell.
                // We know that the nuclues is 0 since we just made the list
                int  i0 = inv_renum[j->a[0]];
                char buf[256];
                snprintf(buf, sizeof(buf), "Uninitialized inv_renum entry for atom %d (%d) shell %d (%d)",
                         j->a[0], inv_renum[j->a[0]],
                         j->a[1], inv_renum[j->a[1]]);
                GMX_RELEASE_ASSERT(i0 >= 0, buf);
                for (int j0 = 0; (j0 < excls_[i0].nr); j0++)
                {
                    add_excl_pair(newexcls, j->a[0], renum[excls_[i0].e[j0]]);
                    add_excl_pair(newexcls, j->a[1], renum[excls_[i0].e[j0]]);
                }
            }
            for (ParamIterator j = pw->beginParam();
                 (j < pw->endParam()); ++j)
            {
                for (int j0 = 0; (j0 < newexcls[j->a[0]].nr); j0++)
                {
                    add_excl_pair(newexcls, j->a[1], newexcls[j->a[0]].e[j0]);
                }
            }
        }
        // Now copy the old atoms to the new structures
        for (i = 0; (i < topology_->atoms.nr); i++)
        {
            newa->atom[renum[i]]      = topology_->atoms.atom[i];
            newa->atomname[renum[i]]  = put_symtab(symtab_, *topology_->atoms.atomname[i]);
            newa->atomtype[renum[i]]  = put_symtab(symtab_, *topology_->atoms.atomtype[i]);
            newa->atomtypeB[renum[i]] = put_symtab(symtab_, *topology_->atoms.atomtypeB[i]);
            copy_rvec(x_[i], newx[renum[i]]);
            newname[renum[i]] = *topology_->atoms.atomtype[i];
            t_atoms_set_resinfo(newa, renum[i], symtab_,
                                *topology_->atoms.resinfo[topology_->atoms.atom[i].resind].name,
                                topology_->atoms.atom[i].resind, ' ', 1, ' ');
        }
        // Now insert the shell particles
        for (i = 0; (i < topology_->atoms.nr); i++)
        {
            iat = renum[i];
            for (j = iat+1; (j < renum[i+1]); j++)
            {
                newa->atom[j]            = topology_->atoms.atom[i];
                newa->atom[iat].q        = pd.getQ(iModel, *topology_->atoms.atomtype[i], 0);
                newa->atom[iat].qB       = newa->atom[iat].q;
                newa->atom[j].m          = 0;
                newa->atom[j].mB         = 0;
                newa->atom[j].atomnumber = 0;
                sprintf(buf, "%s_s", get_atomtype_name(topology_->atoms.atom[i].type,
                                                       atype_));
                newname[j] = strdup(buf);
                shell      = add_atomtype(atype_, symtab_, shell_atom, buf, &p,
                                          0, 0, 0, 0, 0, 0, 0);
                newa->atom[j].type          = shell;
                newa->atom[j].typeB         = shell;
                newa->atomtype[j]           =
                    newa->atomtypeB[j]      = put_symtab(symtab_, buf);
                newa->atom[j].ptype         = eptShell;
                newa->atom[j].q             = pd.getQ(iModel, *topology_->atoms.atomtype[i], 1);
                newa->atom[j].qB            = newa->atom[j].q;
                newa->atom[j].resind        = topology_->atoms.atom[i].resind;
                sprintf(buf, "%ss", *(topology_->atoms.atomname[i]));
                newa->atomname[j] = put_symtab(symtab_, buf);
                copy_rvec(x_[i], newx[j]);
            }
        }
        /* Copy newa to atoms */
        copy_atoms(newa, &topology_->atoms);
        /* Copy coordinates and smnames */
        for (i = 0; (i < newa->nr); i++)
        {
            copy_rvec(newx[i], x_[i]);
            topology_->atoms.atomtype[i] = put_symtab(symtab_, newname[i]);
        }
        sfree(newx);
        sfree(newname);
        /* Copy exclusions, may need to empty the original first */
        sfree(excls_);
        excls_ = newexcls;

        for (auto i = plist_.begin(); (i < plist_.end()); ++i)
        {
            if (i->getFtype() != F_POLARIZATION)
            {
                for (auto j = i->beginParam(); (j < i->endParam()); ++j)
                {
                    for (k = 0; (k < NRAL(i->getFtype())); k++)
                    {
                        j->a[k] = renum[j->a[k]];
                    }
                }
            }
        }
        bHaveShells_ = true;
        sfree(shell_atom);
    }
}

immStatus MyMol::GenerateChargeGroups(eChargeGroup ecg, bool bUsePDBcharge)
{
    real qtot, mtot;

    if ((cgnr_ = generate_charge_groups(ecg, &topology_->atoms,
                                        plist_,
                                        bUsePDBcharge,
                                        &qtot, &mtot)) == nullptr)
    {
        return immChargeGeneration;
    }

    if (ecg != ecgAtom)
    {
        //sort_on_charge_groups(cgnr_, &topology_->atoms,
        //                    plist_, x_, excls_, ndxfn, nmol);
    }
    return immOK;
}

void MyMol::GenerateCube(ChargeDistributionModel iChargeDistributionModel,
                         const Poldata          &pd,
                         real                    spacing,
                         const char             *reffn,
                         const char             *pcfn,
                         const char             *pdbdifffn,
                         const char             *potfn,
                         const char             *rhofn,
                         const char             *hisfn,
                         const char             *difffn,
                         const char             *diffhistfn,
                         const gmx_output_env_t *oenv)
{
    if ((nullptr  != potfn) || (nullptr != hisfn) || (nullptr != rhofn) ||
        ((nullptr != difffn) && (nullptr != reffn)))
    {
        char      buf[256];
        char     *gentop_version = (char *)"v0.99b";
        QgenResp  grref;

        gr_.potcomp(pcfn, pdbdifffn, oenv);

        /* This has to be done before the grid is f*cked up by
           writing a cube file */
        grref = gr_;

        sprintf(buf, "Potential generated by %s based on %s charges",
                gentop_version,
                getEemtypeName(iChargeDistributionModel));

        if (nullptr != difffn)
        {
            grref.setAtomInfo(&topology_->atoms, pd, x_);
            grref.setAtomSymmetry(symmetric_charges_);
            grref.readCube(reffn, FALSE);
            gr_ = grref;
        }
        else
        {
            gr_.makeGrid(spacing, box_, x_);
        }
        if (nullptr != rhofn)
        {
            sprintf(buf, "Electron density generated by %s based on %s charges",
                    gentop_version, getEemtypeName(iChargeDistributionModel));
            gr_.calcRho();
            gr_.writeRho(rhofn, buf);
        }
        sprintf(buf, "Potential generated by %s based on %s charges",
                gentop_version, getEemtypeName(iChargeDistributionModel));
        if (nullptr != potfn)
        {
            gr_.calcPot();
            gr_.writeCube(potfn, buf);
        }
        if (nullptr != hisfn)
        {
            gr_.writeHisto(hisfn, buf, oenv);
        }
        if ((nullptr != difffn) || (nullptr != diffhistfn))
        {
            sprintf(buf, "Potential difference generated by %s based on %s charges",
                    gentop_version,
                    getEemtypeName(iChargeDistributionModel));

            gr_.writeDiffCube(grref, difffn, diffhistfn, buf, oenv, 0);
        }
    }
}

immStatus MyMol::getExpProps(gmx_bool bQM, gmx_bool bZero, char *lot,
                             const Poldata &pd)
{
    immStatus    imm = immOK;
    unsigned int m, nwarn = 0;
    double       value, Hatom, T = -1, error, vec[3];
    tensor       quadrupole;
    std::string  myref, mylot;
    int          ia;

    if (molProp()->getPropRef(MPO_DIPOLE, (bQM ? iqmQM : iqmBoth),
                              lot, nullptr, (char *)"elec",
                              &value, &error, &T, myref, mylot,
                              vec, quadrupole))
    {
        if (!bZero)
        {
            imm = immZeroDip;
        }
    }
    else
    {
        dip_exp  = value;
        dip_err  = error;
        for (m = 0; (m < DIM); m++)
        {
            mu_exp[m] = vec[m];
        }
        mu_exp2 = gmx::square(value);
        if (error <= 0)
        {
            if (debug)
            {
                fprintf(debug, "WARNING: Error for %s is %g, assuming it is 10%%.\n",
                        molProp()->getMolname().c_str(), error);
            }
            nwarn++;
            error = 0.1*value;
        }
        dip_weight = gmx::square(1.0/error);
    }
    /* Check handling of LOT */
    if (molProp()->getPropRef(MPO_DIPOLE, iqmQM,
                              (char *)mylot.c_str(), nullptr, (char *)"ESP", &value, &error, &T,
                              myref, mylot, vec, quadrupole))
    {
        for (m = 0; (m < DIM); m++)
        {
            mu_esp[m] = vec[m];
        }
    }
    if (molProp()->getProp(MPO_ENERGY, (bQM ? iqmQM : iqmBoth),
                           lot, nullptr, (char *)"DeltaHform", &value, &error, &T))
    {
        Hform = value;
        Emol  = value;
        for (ia = 0; (ia < topology_->atoms.nr); ia++)
        {
            if (pd.getAtypeRefEnthalpy(*topology_->atoms.atomtype[ia], &Hatom))
            {
                Emol -= Hatom;
            }
            else
            {
                fprintf(debug, "WARNING: NO ref enthalpy for molecule %s.\n",
                        molProp()->getMolname().c_str());
                Emol = 0;
                break;
            }
        }
        if (ia < topology_->atoms.nr)
        {
            imm = immNoData;
        }
    }
    else
    {
        imm = immNoData;
    }
    return imm;
}

void MyMol::CalcQPol(const Poldata &pd)

{
    int     i, m, np;
    double  poltot, pol, sigpol, sptot, ereftot, eref;
    rvec    mu;

    poltot  = 0;
    sptot   = 0;
    ereftot = 0;
    np      = 0;
    clear_rvec(mu);
    for (i = 0; (i < topology_->atoms.nr); i++)
    {
        if (pd.getAtypePol(*topology_->atoms.atomtype[i], &pol, &sigpol))
        {
            np++;
            poltot += pol;
            sptot  += gmx::square(sigpol);
        }
        if (1 ==
            pd.getAtypeRefEnthalpy(*topology_->atoms.atomtype[i], &eref))
        {
            ereftot += eref;
        }
        for (m = 0; (m < DIM); m++)
        {
            mu[m] += x_[i][m]*topology_->atoms.atom[i].q;
        }
    }
    mutot_          = ENM2DEBYE*norm(mu);
    ref_enthalpy_   = ereftot;
    polarizability_ = poltot;
    sig_pol_        = sqrt(sptot/topology_->atoms.nr);
}

void MyMol::UpdateIdef(const Poldata   &pd,
                       InteractionType  iType)
{
    int          gt;
    std::string  aai, aaj, aak, aal, params;
    int          lu;
    double       value;

    switch (iType)
    {
        case InteractionType_BONDS:
        {
            for (GtBondsConstIterator gtbs = pd.getBondsBegin();
                 gtbs != pd.getBondsEnd(); gtbs++)
            {
                lu = string2unit(gtbs->getLengthUnit().c_str());
                if (-1 == lu)
                {
                    gmx_fatal(FARGS, "Unknown length unit '%s' for bonds",
                              gtbs->getLengthUnit().c_str());
                }
                int ftb = gtbs->getBondFtype();
                for (int i = 0; (i < ltop_->idef.il[ftb].nr); i += interaction_function[ftb].nratoms+1)
                {
                    int         tp  = ltop_->idef.il[ftb].iatoms[i];
                    int         ai  = ltop_->idef.il[ftb].iatoms[i+1];
                    int         aj  = ltop_->idef.il[ftb].iatoms[i+2];
                    std::string aai, aaj;
                    if (pd.atypeToBtype(*topology_->atoms.atomtype[ai], aai) &&
                        pd.atypeToBtype(*topology_->atoms.atomtype[aj], aaj))
                    {
                        /* Here unfortunately we need a case statement for the types */
                        double sigma, bondorder;
                        int    ntrain;
                        if (pd.searchBond(aai, aaj, &value, &sigma, &ntrain, &bondorder, params))
                        {
                            mtop_->ffparams.iparams[tp].morse.b0A = convert2gmx(value, lu);

                            std::vector<std::string> ptr = gmx::splitString(params);
                            int n = 0;
                            for (std::vector<std::string>::iterator pi = ptr.begin(); (pi < ptr.end()); ++pi)
                            {
                                if (pi->length() > 0)
                                {
                                    if (n == 0)
                                    {
                                        mtop_->ffparams.iparams[tp].morse.cbA = atof(pi->c_str());
                                    }
                                    else
                                    {
                                        mtop_->ffparams.iparams[tp].morse.betaA = atof(pi->c_str());
                                    }
                                    n++;
                                }
                            }
                        }
                    }
                    else
                    {
                        gmx_fatal(FARGS, "There are no parameters for bond %s-%s in the force field", aai.c_str(), aaj.c_str());
                    }
                }
            }
        }
        break;
        case InteractionType_ANGLES:
        {
            for (GtAnglesConstIterator gtas = pd.getAnglesBegin();
                 gtas != pd.getAnglesEnd(); gtas++)
            {
                int fta = gtas->getAngleFtype();
                for (int i = 0; (i < ltop_->idef.il[fta].nr); i += interaction_function[fta].nratoms+1)
                {
                    int         tp  = ltop_->idef.il[fta].iatoms[i];
                    int         ai  = ltop_->idef.il[fta].iatoms[i+1];
                    int         aj  = ltop_->idef.il[fta].iatoms[i+2];
                    int         ak  = ltop_->idef.il[fta].iatoms[i+3];
                    std::string aai, aaj, aak;
                    if (pd.atypeToBtype(*topology_->atoms.atomtype[ai], aai) &&
                        pd.atypeToBtype(*topology_->atoms.atomtype[aj], aaj) &&
                        pd.atypeToBtype(*topology_->atoms.atomtype[ak], aak))
                    {
                        double sigma;
                        int    ntrain;
                        if (pd.searchAngle( aai, aaj, aak, &value,
                                            &sigma, &ntrain, params))
                        {
                            mtop_->ffparams.iparams[tp].harmonic.rA     =
                                mtop_->ffparams.iparams[tp].harmonic.rB = value;
                            auto ptr = gmx::splitString(params);
                            for (std::vector<std::string>::iterator pi = ptr.begin(); (pi < ptr.end()); ++pi)
                            {
                                if (pi->length() > 0)
                                {
                                    mtop_->ffparams.iparams[tp].harmonic.krA     =
                                        mtop_->ffparams.iparams[tp].harmonic.krB = atof(pi->c_str());
                                }
                            }
                        }
                    }
                    else
                    {
                        gmx_fatal(FARGS, "here are no parameters for angle %s-%s-%s in the force field", aai.c_str(), aaj.c_str(), aak.c_str());
                    }
                }
            }
        }
        break;
        case InteractionType_PDIHS:
        case InteractionType_IDIHS:
        {
            for (GtDihedralsConstIterator gtds = pd.getDihedralsBegin();
                 gtds != pd.getDihedralsEnd(); gtds++)
            {
                int ftd  = gtds->getDihedralFtype();
                for (int i = 0; (i < ltop_->idef.il[ftd].nr); i += interaction_function[ftd].nratoms+1)
                {
                    int         tp  = ltop_->idef.il[ftd].iatoms[i];
                    int         ai  = ltop_->idef.il[ftd].iatoms[i+1];
                    int         aj  = ltop_->idef.il[ftd].iatoms[i+2];
                    int         ak  = ltop_->idef.il[ftd].iatoms[i+3];
                    int         al  = ltop_->idef.il[ftd].iatoms[i+4];
                    std::string aai, aaj, aak;
                    if (pd.atypeToBtype(*topology_->atoms.atomtype[ai], aai) &&
                        pd.atypeToBtype(*topology_->atoms.atomtype[aj], aaj) &&
                        pd.atypeToBtype(*topology_->atoms.atomtype[ak], aak) &&
                        pd.atypeToBtype(*topology_->atoms.atomtype[al], aal))
                    {
                        double sigma;
                        int    ntrain;
                        if ((gt = pd.searchDihedral(aai, aaj, aak, aal,
                                                    &value, &sigma, &ntrain, params)) != 0)
                        {
                            std::vector<std::string> ptr = gmx::splitString(params);
                            if (ftd == F_PDIHS)
                            {
                                mtop_->ffparams.iparams[tp].pdihs.phiA = value;

                                if (ptr[0].length() > 0)
                                {
                                    mtop_->ffparams.iparams[tp].pdihs.cpA     =
                                        mtop_->ffparams.iparams[tp].pdihs.cpB =
                                            atof(ptr[0].c_str());
                                }
                                if (ptr[1].length() > 0)
                                {
                                    mtop_->ffparams.iparams[tp].pdihs.mult = atof(ptr[1].c_str());
                                }
                            }
                            else
                            {
                                mtop_->ffparams.iparams[tp].harmonic.rA     =
                                    mtop_->ffparams.iparams[tp].harmonic.rB = value;
                                if (ptr[0].length() > 0)
                                {
                                    mtop_->ffparams.iparams[tp].harmonic.krA     =
                                        mtop_->ffparams.iparams[tp].harmonic.krB = atof(ptr[0].c_str());
                                }
                            }
                        }
                    }
                    else
                    {
                        gmx_fatal(FARGS, "There are no parameters for angle %s-%s-%s in the force field", aai.c_str(), aaj.c_str(), aak.c_str());
                    }
                }
            }
        }
        break;
        case InteractionType_LJ14:
        case InteractionType_Polarization:
        case InteractionType_VSITE2:
        case InteractionType_LINEAR_ANGLES:
        case InteractionType_CONSTR:
            break;
    }
}

}