/*  This file is part of aither.
    Copyright (C) 2015-18  Michael Nucci (mnucci@pm.me)

    Aither is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Aither is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <iostream>               // cout, cerr, endl
#include <vector>
#include "linearSolver.hpp"
#include "procBlock.hpp"
#include "input.hpp"
#include "matMultiArray3d.hpp"
#include "physicsModels.hpp"
#include "gridLevel.hpp"
#include "utility.hpp"

using std::cout;
using std::endl;
using std::cerr;
using std::vector;

// function declarations

blkMultiArray3d<varArray> linearSolver::InitializeMatrixUpdate(
    const procBlock &blk, const input &inp, const physics &phys,
    const matMultiArray3d &aInv) const {
  // blk -- procBlock associated with diagonal
  // inp -- input variables
  // phys -- physics models
  // aInv -- inverse of main diagonal

  // allocate multiarray for update
  blkMultiArray3d<varArray> x(blk.NumI(), blk.NumJ(), blk.NumK(),
                              blk.NumGhosts(), inp.NumEquations(),
                              inp.NumSpecies(), 0.0);

  if (inp.MatrixRequiresInitialization()) {
    const auto thetaInv = 1.0 / inp.Theta();

    for (auto kk = blk.StartK(); kk < blk.EndK(); ++kk) {
      for (auto jj = blk.StartJ(); jj < blk.EndJ(); ++jj) {
        for (auto ii = blk.StartI(); ii < blk.EndI(); ++ii) {
          // calculate update
          x.InsertBlock(
              ii, jj, kk,
              aInv.ArrayMult(ii, jj, kk,
                             -thetaInv * blk.Residual(ii, jj, kk) +
                                 blk.SolDeltaNm1(ii, jj, kk, inp) -
                                 blk.SolDeltaMmN(ii, jj, kk, inp, phys)));
        }
      }
    }
  }
  return x;
}

void linearSolver::InvertDiagonal(const procBlock &blk, const input &inp,
                                  matMultiArray3d &mainDiagonal) const {
  // blk -- block associated with mainDiagonal
  // inp -- input variables
  // mainDiagonal -- main diagonal in implicit operator

  // loop over physical cells
  for (auto kk = blk.StartK(); kk < blk.EndK(); ++kk) {
    for (auto jj = blk.StartJ(); jj < blk.EndJ(); ++jj) {
      for (auto ii = blk.StartI(); ii < blk.EndI(); ++ii) {
        auto diagVolTime = blk.SolDeltaNCoeff(ii, jj, kk, inp);
        if (inp.DualTimeCFL() > 0.0) {  // use dual time stepping
          // equal to volume / tau
          diagVolTime += blk.SpectralRadius(ii, jj, kk).Max() /
              inp.DualTimeCFL();
        }

        // add volume and time term
        mainDiagonal.MultiplyOnDiagonal(ii, jj, kk, inp.MatrixRelaxation());
        mainDiagonal.AddOnDiagonal(ii, jj, kk, diagVolTime);
        mainDiagonal.Inverse(ii, jj, kk);
      }
    }
  }
}

lusgs::lusgs(const string &type, const gridLevel &level) : linearSolver(type) {
  // calculate order by hyperplanes for each block
  reorder_.resize(level.NumBlocks());
  for (auto bb = 0; bb < level.NumBlocks(); ++bb) {
    reorder_[bb] =
        HyperplaneReorder(level.Block(bb).NumI(), level.Block(bb).NumJ(),
                          level.Block(bb).NumK());
  }
}

/* Member function to calculate update to solution implicitly using Lower-Upper
Symmetric Gauss Seidel (LUSGS) method.

Un+1 = Un - t/V * Rn+1

The above equation shows a simple first order implicit method to calculate the
solution at the next time step (n+1). The equation shows that this method
requires the residual (R) at time n+1 which is unknown. In the equation, t is
the time step, and V is the volume. Since the residual at n+1 in unknown, it
must be linearized about time n as shown below.

Rn+1 = Rn + dRn/dUn * FD(Un)

In the above equation FD is the forward difference operator in time (FD(Un) =
Un+1 - Un). The derivative of the residual term can be further simplified as
below.

Rn = (sum over all faces) Fni

In the above equation n refers to the time level and i refers to the face index.
The summation over all faces operator will now be abbreviated as (SF).
Substituting the second and third equations into the first and rearranging we
get the following.

[d(SF)Fni/dUnj + V/t] * FD(Un) = -Rn

In the above equation the index j refers to all of the cells in the stencil
going into the calculation of the flux at face i. The above equation
can be simplified to A*x=b. The matrix A is an MxM block matrix where M is the
number of cells in the block. Each block is an LxL block where L
is the number of equations being solved for. The sparsity of A depends on the
stencil used in flux calculation. In 3D for a first order simulation
the matrix A is block pentadiagonal. For a second order approximation it would
have 13 diagonals. This increases the storage requirements so in
practice a first order approximation is used. The order of accuracy is
determined by the residual calculation. The accuracy of the matrix A helps
with convergence. A poorer approximation will eventually get the correct answer
with enough iteration (defect correction). Fully implicit methods
calculate and store the flux jacobians needed to populate the matrix A. The
LUSGS method does not do this and instead calculates an approximate
flux jacobian "on-the-fly" so there is no need for storage. Because an
approximate flux jacobian is being used, there is no need for a highly
accurate linear solver. Therefore the Symmetric Gauss-Seidel (SGS) method is a
good candidate, and only one iteration is needed. This approximate
flux jacobian along with the SGS linear solver form the basics of the LUSGS
method (Jameson & Yoon). The LUSGS method begins by factoring the
matrix A as shown below.

A = (D + L) * D^-1 * (D + U)

In the above equation D is the diagonal of A, L is the lower triangular portion
of A, and U is the upper triangular portion of A. This allows the equation A*x=b
to be solved in one SGS sweep as shown below.

Forward sweep:  (D + L) * FD(Un*) = -Rn
Backward sweep: (D + U) * FD(Un) = D * FD(Un*)

Another key component of the LUSGS scheme is to sweep along hyperplanes.
Hyperplanes are planes of i+j+k=constant within a plot3d block. The diagram
below shows a 2D example of a block reordered to sweep along hyperplanes
           ____ ____ ____ ____ ____ ____ ____ ____
          | 20 | 26 | 32 | 37 | 41 | 44 | 46 | 47 |
          |____|____|____|____|____|____|____|____|
          | 14 | 19 | 25 | 31 | 36 | 40 | 43 | 45 |
          |____|____|____|____|____|____|____|____|
          | 9  | 13 | 18 | 24 | 30 | 35 | 39 | 42 |
   A=     |____|____|____|____|____|____|____|____|
          | 5  | 8  | 12 | 17 | 23 | 29 | 34 | 38 |
          |____|____|____|____|____|____|____|____|
          | 2  | 4  | 7  | 11 | 16 | 22 | 28 | 33 |
          |____|____|____|____|____|____|____|____|
          | 0  | 1  | 3  | 6  | 10 | 15 | 21 | 27 |
          |____|____|____|____|____|____|____|____|

This is advantageous because on the forward sweep the lower matrix L can be
calculated with data from time n+1 because all of the cells contributing
to L would already have been updated. The same is true with the upper matrix U
for the backward sweep. This removes the need for any storage of the
matrix. For a given location (say A12) the matrix L would be constructed of A8
and A7, and the matrix U would be constructed of A18 and A17. This
requires the product of the flux jacobian multiplied with the update (FD(Un)) to
be calculated at these locations. For the LUSGS method the flux
jacobians are approximated as follows:

A * S = 0.5 * (Ac * S + K * I)

A is the flux jacobian, S is the face area, Ac is the convective flux jacobian
(dF/dU), K is the spectral radius multiplied by a factor, and I is
the identity matrix. The addition of the spectrial radius improves diagonal
dominance which improves stability at the cost of convergence. When the
factor multiplied by K is 1, the method is SGS, when it is < 1, it is successive
overrelaxation. Reducing the factor improves convergence but hurts
stability. The product of the approximate flux jacobian with the update is
calculated as shown below.

A * S * FD(Unj) = 0.5 * (Ac * FD(Unj) * S + K * I * FD(Unj)) = 0.5 * ( dFi/dUnj
* FD(Unj) * S + K * I * FD(Unj)) = 0.5 * (dFi * S + K * I * FD(Unj))

The above equation shows that all that is needed to calculate the RHS of A*x=b
is the update of the convective flux, and the update to the convervative
variabes (FD(Unj)) which is known due to sweeping along hyperplanes.

For viscous simulations, the viscous contribution to the spectral radius K is
used, and everything else remains the same.
 */
void lusgs::LUSGS_Forward(const procBlock &blk,
                          const vector<vector3d<int>> &reorder,
                          const physics &phys, const input &inp,
                          const matMultiArray3d &aInv, const int &sweep,
                          blkMultiArray3d<varArray> &x) const {
  // blk -- block to solve on
  // reorder -- order of cells to visit (this should be ordered in hyperplanes)
  // phys -- physics models
  // inp -- all input variables
  // aInv -- inverse of main diagonal
  // sweep -- sweep number through domain
  // x -- variables to be solved for

  const auto thetaInv = 1.0 / inp.Theta();

  //--------------------------------------------------------------------
  // forward sweep over all physical cells
  for (auto nn = 0; nn < blk.NumCells(); ++nn) {
    // indices for variables without ghost cells
    const auto ii = reorder[nn].X();
    const auto jj = reorder[nn].Y();
    const auto kk = reorder[nn].Z();

    // calculate lower and upper off diagonals on the fly
    // normal at lower boundaries needs to be reversed, so add instead
    // of subtract L
    auto offDiagonal = blk.ImplicitLower(ii, jj, kk, x, phys, inp);
    if (sweep > 0 || inp.MatrixRequiresInitialization()) {
      offDiagonal -= blk.ImplicitUpper(ii, jj, kk, x, phys, inp);
    }

    // calculate 'b' terms - these change at subiteration level
    const auto solDeltaNm1 = blk.SolDeltaNm1(ii, jj, kk, inp);
    const auto solDeltaMmN = blk.SolDeltaMmN(ii, jj, kk, inp, phys);
    const auto b =
        -thetaInv * blk.Residual(ii, jj, kk) + solDeltaNm1 - solDeltaMmN;

    // calculate intermediate update
    x.InsertBlock(ii, jj, kk, aInv.ArrayMult(ii, jj, kk, b + offDiagonal));
  }  // end forward sweep
}

double lusgs::LUSGS_Backward(const procBlock &blk,
                             const vector<vector3d<int>> &reorder,
                             const physics &phys, const input &inp,
                             const matMultiArray3d &aInv, const int &sweep,
                             blkMultiArray3d<varArray> &x) const {
  // blk -- block to solve on
  // reorder -- order of cells to visit (this should be ordered in hyperplanes)
  // phys -- physics models
  // inp -- all input variables
  // aInv -- inverse of main diagonal
  // sweep -- sweep number through domain
  // x -- variables to be solved for

  const auto thetaInv = 1.0 / inp.Theta();

  varArray l2Error(inp.NumEquations(), inp.NumSpecies());

  // backward sweep over all physical cells
  for (auto nn = blk.NumCells() - 1; nn >= 0; --nn) {
    // indices for variables without ghost cells
    const auto ii = reorder[nn].X();
    const auto jj = reorder[nn].Y();
    const auto kk = reorder[nn].Z();

    // calculate upper off diagonals on the fly
    const auto U = blk.ImplicitUpper(ii, jj, kk, x, phys, inp);

    // calculate update
    const auto xold = x.GetCopy(ii, jj, kk);
    if (sweep > 0 || inp.MatrixRequiresInitialization()) {
      const auto L = blk.ImplicitLower(ii, jj, kk, x, phys, inp);
          // calculate 'b' terms - these change at subiteration level
      const auto solDeltaNm1 = blk.SolDeltaNm1(ii, jj, kk, inp);
      const auto solDeltaMmN = blk.SolDeltaMmN(ii, jj, kk, inp, phys);
      const auto b =
          -thetaInv * blk.Residual(ii, jj, kk) + solDeltaNm1 - solDeltaMmN;
      x.InsertBlock(ii, jj, kk, aInv.ArrayMult(ii, jj, kk, b + L - U));
    } else {
      x.InsertBlock(ii, jj, kk, xold - aInv.ArrayMult(ii, jj, kk, U));
    }
    const auto error = x(ii, jj, kk) - xold;
    l2Error += error * error;
  }  // end backward sweep

  return l2Error.Sum();
}

double lusgs::Relax(const gridLevel &level, const physics &phys,
                    const input &inp, const int &rank, const int &sweeps,
                    vector<blkMultiArray3d<varArray>> &du) const {
  MSG_ASSERT(level.NumBlocks() == static_cast<int>(du.size()),
             "number of blocks mismatch");
  MSG_ASSERT(du.size() == reorder_.size(), "reorder block size mismatch");

  // initialize matrix error
  auto matrixError = 0.0;

  const auto numG = level.Block(0).NumGhosts();

  // start sweeps through domain
  for (auto ii = 0; ii < sweeps; ++ii) {
    // swap updates for ghost cells
    SwapImplicitUpdate(du, level.Connections(), rank, numG);

    // forward lu-sgs sweep
    for (auto bb = 0; bb < level.NumBlocks(); ++bb) {
      this->LUSGS_Forward(level.Block(bb), reorder_[bb], phys, inp,
                          level.Diagonal(bb), ii, du[bb]);
    }

    // swap updates for ghost cells
    SwapImplicitUpdate(du, level.Connections(), rank, numG);

    // backward lu-sgs sweep
    for (auto bb = 0; bb < level.NumBlocks(); ++bb) {
      matrixError += this->LUSGS_Backward(level.Block(bb), reorder_[bb], phys,
                                          inp, level.Diagonal(bb), ii, du[bb]);
    }
  }
  return matrixError;
}

// function to calculate the implicit update via the DP-LUR method
double dplur::DPLUR(const procBlock &blk, const physics &phys, const input &inp,
                    const matMultiArray3d &aInv,
                    blkMultiArray3d<varArray> &x) const {
  // blk -- block to solve on
  // phys --  physics models
  // inp -- all input variables
  // aInv -- inverse of main diagonal
  // x -- variables to solve for

  const auto thetaInv = 1.0 / inp.Theta();

  // initialize residuals
  varArray l2Error(inp.NumEquations(), inp.NumSpecies());
  // copy old update
  const auto xold = x;

  for (auto kk = blk.StartK(); kk < blk.EndK(); ++kk) {
    for (auto jj = blk.StartJ(); jj < blk.EndJ(); ++jj) {
      for (auto ii = blk.StartI(); ii < blk.EndI(); ++ii) {
        // calculate off diagonal terms on the fly
        auto offDiagonal = blk.ImplicitLower(ii, jj, kk, xold, phys, inp);
        offDiagonal -= blk.ImplicitUpper(ii, jj, kk, xold, phys, inp);
        // calculate 'b' terms - these change at subiteration level
        const auto solDeltaNm1 = blk.SolDeltaNm1(ii, jj, kk, inp);
        const auto solDeltaMmN = blk.SolDeltaMmN(ii, jj, kk, inp, phys);
        const auto b =
            -thetaInv * blk.Residual(ii, jj, kk) + solDeltaNm1 - solDeltaMmN;

        // calculate update
        x.InsertBlock(ii, jj, kk, aInv.ArrayMult(ii, jj, kk, b + offDiagonal));

        // calculate matrix error
        const auto error = x(ii, jj, kk) - xold(ii, jj, kk);
        l2Error += error * error;
      }
    }
  }
  return l2Error.Sum();
}

double dplur::Relax(const gridLevel &level, const physics &phys,
                    const input &inp, const int &rank, const int &sweeps,
                    vector<blkMultiArray3d<varArray>> &du) const {
  MSG_ASSERT(level.NumBlocks() == static_cast<int>(du.size()),
             "number of blocks mismatch");

  // initialize matrix error
  auto matrixError = 0.0;

  const auto numG = level.Block(0).NumGhosts();

  // start sweeps through domain
  for (auto ii = 0; ii < sweeps; ++ii) {
    // swap updates for ghost cells
    SwapImplicitUpdate(du, level.Connections(), rank, numG);

    // dplur sweep
    for (auto bb = 0; bb < level.NumBlocks(); ++bb) {
      this->DPLUR(level.Block(bb), phys, inp, level.Diagonal(bb), du[bb]);
    }
  }
  return matrixError;
}