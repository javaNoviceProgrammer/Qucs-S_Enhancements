/*
 * complex.cpp - complex number class implementation
 *
 * Copyright (C) 2003, 2004, 2005, 2006, 2007 Stefan Jahn <stefan@lkcc.org>
 * Copyright (C) 2006 Gunther Kraut <gn.kraut@t-online.de>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * $Id$
 *
 */

/*!\file complex.cpp
   Implements complex number class and functions
*/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <cmath>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "precision.h"
#include "complex.h"
#include "consts.h"
#include "fspecial.h"


namespace qucs {



//using namespace fspecial;


// === bessel ===

/* FIXME : what about libm jn, yn, isn't that enough? */

nr_complex_t cbesselj (unsigned int, nr_complex_t);

#include "cbesselj.cpp"

/*!\brief Bessel function of first kind

   \param[in] n order
   \param[in] z argument
   \return Bessel function of first kind of order n
   \bug Not implemented
*/
nr_complex_t jn (const int n, const nr_complex_t z)
{
    return cbesselj (n, z);
}


/*!\brief Bessel function of second kind

   \param[in] n order
   \param[in] z argument
   \return Bessel function of second kind of order n
   \bug Not implemented
*/
nr_complex_t yn (const int n, const nr_complex_t z)
{
    return nr_complex_t (::yn (n, std::real (z)), 0);
}


/*!\brief Modified Bessel function of first kind

   \param[in] z argument
   \return Modified Bessel function of first kind of order 0
   \bug Not implemented
*/
nr_complex_t i0 (const nr_complex_t z)
{
    return nr_complex_t (fspecial::i0 (std::real (z)), 0);
}

/*!\brief Inverse of error function

   \param[in] z argument
   \return Inverse of error function
   \bug Not implemented
*/
nr_complex_t erfinv (const nr_complex_t z)
{
    return nr_complex_t (fspecial::erfinv (std::real (z)), 0);
}

/*!\brief Inverse of complementart error function

   \param[in] z argument
   \return Inverse of complementary error function
   \bug Not implemented
*/
nr_complex_t erfcinv (const nr_complex_t z)
{
    return nr_complex_t (fspecial::erfcinv (std::real (z)), 0);
}

} // namespace qucs

