/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2010,2014,2015,2018, by the GROMACS development team, led by
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
#ifndef GMX_TOPOLOGY_BLOCK_H
#define GMX_TOPOLOGY_BLOCK_H

#include <stdio.h>

#ifdef __cplusplus
#include <vector>
#endif

#include "gromacs/utility/basedefinitions.h"
#ifdef __cplusplus
#include "gromacs/utility/gmxassert.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
namespace gmx
{

/*! \brief Division of a range of indices into consecutive blocks
 *
 * A range of consecutive indices 0 to index[numBlocks()] is divided
 * into numBlocks() consecutive blocks of consecutive indices.
 * Block b contains indices i for which index[b] <= i < index[b+1].
 */
struct BlockRanges
{
    /*! \brief Returns the number of blocks
     *
     * This should only be called on a valid struct. Validy is asserted
     * (only) in debug mode.
     */
    int numBlocks() const
    {
        GMX_ASSERT(index.size() > 0, "numBlocks() should only be called on a valid BlockRanges struct");
        return index.size() - 1;
    }

    std::vector<int> index; /**< The list of block begin/end indices */
};

}      // nsamespace gmx
#endif // __cplusplus

/* Deprecated, C-style version of BlockRanges */
typedef struct t_block
{
    int      nr;           /* The number of blocks          */
    int     *index;        /* Array of indices (dim: nr+1)  */
    int      nalloc_index; /* The allocation size for index */
} t_block;

typedef struct t_blocka
{
    int      nr;    /* The number of blocks              */
    int     *index; /* Array of indices in a (dim: nr+1) */
    int      nra;   /* The number of atoms               */
    int     *a;     /* Array of atom numbers in each group  */
    /* (dim: nra)                           */
    /* Block i (0<=i<nr) runs from          */
    /* index[i] to index[i+1]-1. There will */
    /* allways be an extra entry in index   */
    /* to terminate the table               */
    int nalloc_index;           /* The allocation size for index        */
    int nalloc_a;               /* The allocation size for a            */
} t_blocka;

void init_block(t_block *block);
void init_blocka(t_blocka *block);
t_blocka *new_blocka(void);
/* allocate new block */

void done_block(t_block *block);
void done_blocka(t_blocka *block);

void copy_blocka(const t_blocka *src, t_blocka *dest);

void stupid_fill_block(t_block *grp, int natom, gmx_bool bOneIndexGroup);
/* Fill a block structure with numbers identical to the index
 * (0, 1, 2, .. natom-1)
 * If bOneIndexGroup, then all atoms are  lumped in one index group,
 * otherwise there is one atom per index entry
 */

void stupid_fill_blocka(t_blocka *grp, int natom);
/* Fill a block structure with numbers identical to the index
 * (0, 1, 2, .. natom-1)
 * There is one atom per index entry
 */

void pr_block(FILE *fp, int indent, const char *title, const t_block *block, gmx_bool bShowNumbers);
void pr_blocka(FILE *fp, int indent, const char *title, const t_blocka *block, gmx_bool bShowNumbers);

#ifdef __cplusplus
}
#endif

#endif
