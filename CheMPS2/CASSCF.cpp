/*
   CheMPS2: a spin-adapted implementation of DMRG for ab initio quantum chemistry
   Copyright (C) 2013-2016 Sebastian Wouters

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <math.h>
#include <algorithm>

#include "CASSCF.h"
#include "Lapack.h"

using std::string;
using std::ifstream;
using std::cout;
using std::endl;
using std::max;

CheMPS2::CASSCF::CASSCF(Hamiltonian * ham_in, int * docc_in, int * socc_in, int * nocc_in, int * ndmrg_in, int * nvirt_in){

   HamOrig = ham_in;
   
   L = HamOrig->getL();
   SymmInfo.setGroup(HamOrig->getNGroup());
   num_irreps = SymmInfo.getNumberOfIrreps();
   
   DOCC = new int[ num_irreps ];
   SOCC = new int[ num_irreps ];
   
   for ( int irrep = 0; irrep < num_irreps; irrep++ ){
      DOCC[ irrep ] = docc_in[ irrep ];
      SOCC[ irrep ] = socc_in[ irrep ];
   }
   
   cout << "DOCC = [ ";
   for ( int irrep = 0; irrep < num_irreps; irrep++ ){ cout << DOCC[ irrep ] << " , "; }
   cout << DOCC[ num_irreps - 1 ] << " ]" << endl;
   cout << "SOCC = [ ";
   for ( int irrep = 0; irrep < num_irreps; irrep++ ){ cout << SOCC[ irrep ] << " , "; }
   cout << SOCC[ num_irreps - 1 ] << " ]" << endl;
   
   iHandler = new DMRGSCFindices( L, SymmInfo.getGroupNumber(), nocc_in, ndmrg_in, nvirt_in );
   unitary  = new DMRGSCFunitary( iHandler );
   theDIIS = NULL;
   theRotatedTEI = new DMRGSCFintegrals( iHandler );
   
   //Allocate space for the DMRG 1DM and 2DM
   nOrbDMRG = iHandler->getDMRGcumulative( num_irreps );
   DMRG1DM = new double[nOrbDMRG * nOrbDMRG];
   DMRG2DM = new double[nOrbDMRG * nOrbDMRG * nOrbDMRG * nOrbDMRG];
   
   //To calculate the F-matrix and Q-matrix(occ,act) elements only once, and to store them for future access
   theFmatrix = new DMRGSCFmatrix( iHandler );  theFmatrix->clear();
   theQmatOCC = new DMRGSCFmatrix( iHandler );  theQmatOCC->clear();
   theQmatACT = new DMRGSCFmatrix( iHandler );  theQmatACT->clear();
   theQmatWORK= new DMRGSCFmatrix( iHandler ); theQmatWORK->clear();
   theTmatrix = new DMRGSCFmatrix( iHandler );  theTmatrix->clear();
   
   //To calculate the w_tilde elements only once, and store them for future access
   wmattilde = new DMRGSCFwtilde( iHandler );
   
   //Print the MO info. This requires the indexHandler to be created...
   checkHF();
   
   //Print what we have just set up.
   iHandler->Print();
   
   cout << "DMRGSCF::setupStart : Number of variables in the x-matrix = " << unitary->getNumVariablesX() << endl;

}

CheMPS2::CASSCF::~CASSCF(){
   
   delete [] DOCC;
   delete [] SOCC;
   
   delete theRotatedTEI;

   delete [] DMRG1DM;
   delete [] DMRG2DM;
   
   //The following objects depend on iHandler: delete them first
   delete theFmatrix;
   delete theQmatOCC;
   delete theQmatACT;
   delete theQmatWORK;
   delete theTmatrix;
   delete wmattilde;
   delete unitary;
   
   delete iHandler;
   if (theDIIS!=NULL){ delete theDIIS; }

}

int CheMPS2::CASSCF::get_num_irreps(){ return num_irreps; }

void CheMPS2::CASSCF::copy2DMover(TwoDM * theDMRG2DM, const int totOrbDMRG, double * localDMRG2DM){

   for (int i1=0; i1<totOrbDMRG; i1++){
      for (int i2=0; i2<totOrbDMRG; i2++){
         for (int i3=0; i3<totOrbDMRG; i3++){
            for (int i4=0; i4<totOrbDMRG; i4++){
               // The assignment has been changed to an addition for state-averaged calculations!
               localDMRG2DM[i1 + totOrbDMRG * ( i2 + totOrbDMRG * (i3 + totOrbDMRG * i4 ) ) ] += theDMRG2DM->getTwoDMA_HAM(i1, i2, i3, i4);
            }
         }
      }
   }

}

void CheMPS2::CASSCF::copy3DMover(ThreeDM * theDMRG3DM, const int numL, double * three_dm){

   for ( int i = 0; i < numL; i++ ){
      for ( int j = 0; j < numL; j++ ){
         for ( int k = 0; k < numL; k++ ){
            for ( int l = 0; l < numL; l++ ){
               for ( int m = 0; m < numL; m++ ){
                  for ( int n = 0; n < numL; n++ ){
                     three_dm[ i + numL * ( j + numL * ( k + numL * ( l + numL * ( m + numL * n ) ) ) ) ] = theDMRG3DM->get_ham_index(i, j, k, l, m, n);
                  }
               }
            }
         }
      }
   }

}

void CheMPS2::CASSCF::setDMRG1DM(const int num_elec, const int numL, double * localDMRG1DM, double * localDMRG2DM){

   const double prefactor = 1.0/( num_elec - 1.0 );

   for ( int cnt1 = 0; cnt1 < numL; cnt1++ ){
      for ( int cnt2 = cnt1; cnt2 < numL; cnt2++ ){
         double value = 0.0;
         for ( int sum = 0; sum < numL; sum++ ){ value += localDMRG2DM[ cnt1 + numL * ( sum + numL * ( cnt2 + numL * sum ) ) ]; }
         localDMRG1DM[ cnt1 + numL * cnt2 ] = prefactor * value;
         localDMRG1DM[ cnt2 + numL * cnt1 ] = localDMRG1DM[ cnt1 + numL * cnt2 ];
      }
   }

}

void CheMPS2::CASSCF::fillLocalizedOrbitalRotations(CheMPS2::DMRGSCFunitary * unitary, CheMPS2::DMRGSCFindices * localIdx, double * eigenvecs){

   const int numIrreps = localIdx->getNirreps();
   const int totOrbDMRG = localIdx->getDMRGcumulative( numIrreps );
   const int size = totOrbDMRG * totOrbDMRG;
   for (int cnt=0; cnt<size; cnt++){ eigenvecs[cnt] = 0.0; }
   int passed = 0;
   for (int irrep=0; irrep<numIrreps; irrep++){

      const int NDMRG = localIdx->getNDMRG(irrep);
      if (NDMRG>0){

         double * blockUnit = unitary->getBlock(irrep);
         double * blockEigs = eigenvecs + passed * ( 1 + totOrbDMRG );

         for (int row=0; row<NDMRG; row++){
            for (int col=0; col<NDMRG; col++){
               blockEigs[row + totOrbDMRG * col] = blockUnit[col + NDMRG * row]; //Eigs = Unit^T
            }
         }

      }

      passed += NDMRG;

   }

}

void CheMPS2::CASSCF::calcNOON(DMRGSCFindices * localIdx, double * eigenvecs, double * workmem, double * localDMRG1DM){

   const int numIrreps = localIdx->getNirreps();
   int totOrbDMRG = localIdx->getDMRGcumulative( numIrreps );
   int size = totOrbDMRG * totOrbDMRG;
   double * eigenval = workmem + size;

   for (int cnt=0; cnt<size; cnt++){ eigenvecs[cnt] = localDMRG1DM[cnt]; }

   char jobz = 'V';
   char uplo = 'U';
   int info;
   int passed = 0;
   for (int irrep=0; irrep<numIrreps; irrep++){

      int NDMRG = localIdx->getNDMRG(irrep);
      if (NDMRG > 0){
         //Calculate the eigenvectors and values per block
         dsyev_(&jobz, &uplo, &NDMRG, eigenvecs + passed*(1+totOrbDMRG), &totOrbDMRG, eigenval + passed, workmem, &size, &info);

         //Sort the eigenvecs
         for (int col=0; col<NDMRG/2; col++){
            for (int row=0; row<NDMRG; row++){
               double temp = eigenvecs[passed + row + totOrbDMRG * (passed + NDMRG - 1 - col)];
               eigenvecs[passed + row + totOrbDMRG * (passed + NDMRG - 1 - col)] = eigenvecs[passed + row + totOrbDMRG * (passed + col)];
               eigenvecs[passed + row + totOrbDMRG * (passed + col)] = temp;
            }
         }
      }

      //Update the number of passed DMRG orbitals
      passed += NDMRG;

   }

}

void CheMPS2::CASSCF::rotate2DMand1DM(const int nDMRGelectrons, int totOrbDMRG, double * eigenvecs, double * work, double * localDMRG1DM, double * localDMRG2DM){

   char notr = 'N';
   char tran = 'T';
   double alpha = 1.0;
   double beta = 0.0;

   int power1 = totOrbDMRG;
   int power2 = totOrbDMRG*totOrbDMRG;
   int power3 = totOrbDMRG*totOrbDMRG*totOrbDMRG;

   //2DM: Gamma_{ijkl} --> Gamma_{ajkl}
   dgemm_(&tran,&notr,&power1,&power3,&power1,&alpha,eigenvecs,&power1,localDMRG2DM,&power1,&beta,work,&power1);
   //2DM: Gamma_{ajkl} --> Gamma_{ajkd}
   dgemm_(&notr,&notr,&power3,&power1,&power1,&alpha,work,&power3,eigenvecs,&power1,&beta,localDMRG2DM,&power3);
   //2DM: Gamma_{ajkd} --> Gamma_{ajcd}
   for (int cnt=0; cnt<totOrbDMRG; cnt++){
      dgemm_(&notr,&notr,&power2,&power1,&power1,&alpha,localDMRG2DM + cnt*power3,&power2,eigenvecs,&power1,&beta,work + cnt*power3,&power2);
   }
   //2DM: Gamma_{ajcd} --> Gamma_{abcd}
   for (int cnt=0; cnt<power2; cnt++){
      dgemm_(&notr,&notr,&power1,&power1,&power1,&alpha,work + cnt*power2,&power1,eigenvecs,&power1,&beta,localDMRG2DM + cnt*power2,&power1);
   }

   //Update 1DM
   setDMRG1DM(nDMRGelectrons, totOrbDMRG, localDMRG1DM, localDMRG2DM);

}

void CheMPS2::CASSCF::rotateOldToNew(DMRGSCFmatrix * myMatrix){

   for ( int irrep = 0; irrep < num_irreps; irrep++ ){
   
      int linsize = iHandler->getNORB(irrep);
      double * Umat = unitary->getBlock(irrep);
      double * work = theQmatWORK->getBlock(irrep);
      double * block = myMatrix->getBlock(irrep);
      double alpha = 1.0;
      double beta  = 0.0;
      char trans   = 'T';
      char notrans = 'N';
      dgemm_(&notrans, &notrans, &linsize, &linsize, &linsize, &alpha, Umat, &linsize, block, &linsize, &beta, work,  &linsize);
      dgemm_(&notrans, &trans,   &linsize, &linsize, &linsize, &alpha, work, &linsize, Umat,  &linsize, &beta, block, &linsize);
      
   }

}

void CheMPS2::CASSCF::constructCoulombAndExchangeMatrixInOrigIndices(DMRGSCFmatrix * densityMatrix, DMRGSCFmatrix * resultMatrix){

  for ( int irrepQ = 0; irrepQ < num_irreps; irrepQ++ ){
   
      const int linearsizeQ = iHandler->getNORB(irrepQ);
      const int numberOfUniqueIndices = (linearsizeQ * (linearsizeQ + 1))/2;
      
      #pragma omp parallel for schedule(static)
      for (int combinedindex = 0; combinedindex < numberOfUniqueIndices; combinedindex++){
      
         int colQ = 1;
         while ( (colQ*(colQ+1))/2 <= combinedindex ){ colQ++; }
         colQ -= 1;
         int rowQ = combinedindex - (colQ*(colQ+1))/2;
         
         const int HamIndexI = iHandler->getOrigNOCCstart(irrepQ) + rowQ;
         const int HamIndexJ = iHandler->getOrigNOCCstart(irrepQ) + colQ;
         
         double theValue = 0.0;
         
         for ( int irrepN = 0; irrepN < num_irreps; irrepN++ ){
            const int linearsizeN = iHandler->getNORB( irrepN );
            for (int rowN = 0; rowN < linearsizeN; rowN++){
            
               const int HamIndexS = iHandler->getOrigNOCCstart( irrepN ) + rowN;
               theValue += densityMatrix->get(irrepN, rowN, rowN) * ( HamOrig->getVmat(HamIndexI,HamIndexS,HamIndexJ,HamIndexS)
                                                              - 0.5 * HamOrig->getVmat(HamIndexI,HamIndexJ,HamIndexS,HamIndexS) );
               
               for (int colN = rowN+1; colN < linearsizeN; colN++){
               
                  const int HamIndexT = iHandler->getOrigNOCCstart( irrepN ) + colN;
                  theValue += densityMatrix->get(irrepN, rowN, colN) * ( 2 * HamOrig->getVmat(HamIndexI,HamIndexS,HamIndexJ,HamIndexT)
                                                                     - 0.5 * HamOrig->getVmat(HamIndexI,HamIndexJ,HamIndexS,HamIndexT) 
                                                                     - 0.5 * HamOrig->getVmat(HamIndexI,HamIndexJ,HamIndexT,HamIndexS) );
               
               }
            }
         }
         
         resultMatrix->set( irrepQ, rowQ, colQ, theValue );
         resultMatrix->set( irrepQ, colQ, rowQ, theValue );
      
      }
   }

}

void CheMPS2::CASSCF::buildQmatOCC(){

   for ( int irrep = 0; irrep < num_irreps; irrep++ ){
   
      int linsize = iHandler->getNORB(irrep);
      int NOCC    = iHandler->getNOCC(irrep);
      double alpha = 2.0;
      double beta  = 0.0;
      char trans   = 'T';
      char notrans = 'N';
      double * Umat = unitary->getBlock(irrep);
      double * work = theQmatWORK->getBlock(irrep);
      dgemm_(&trans, &notrans, &linsize, &linsize, &NOCC, &alpha, Umat, &linsize, Umat, &linsize, &beta, work, &linsize);
      
   }
   
   constructCoulombAndExchangeMatrixInOrigIndices( theQmatWORK, theQmatOCC );
   rotateOldToNew( theQmatOCC );

}

void CheMPS2::CASSCF::buildQmatACT(){

   for ( int irrep = 0; irrep < num_irreps; irrep++ ){
   
      int linsize = iHandler->getNORB(irrep);
      int NDMRG   = iHandler->getNDMRG(irrep);
      double alpha = 1.0;
      double beta  = 0.0;
      char trans   = 'T';
      char notrans = 'N';
      double * Umat  =     unitary->getBlock(irrep) + iHandler->getNOCC(irrep);
      double * work  = theQmatWORK->getBlock(irrep);
      double * work2 =  theQmatACT->getBlock(irrep);
      double * RDM = DMRG1DM + iHandler->getDMRGcumulative(irrep) * ( 1 + nOrbDMRG );
      dgemm_(&trans,   &notrans, &linsize, &NDMRG,   &NDMRG, &alpha, Umat,  &linsize, RDM,  &nOrbDMRG, &beta, work2, &linsize);
      dgemm_(&notrans, &notrans, &linsize, &linsize, &NDMRG, &alpha, work2, &linsize, Umat, &linsize,  &beta, work,  &linsize);
      
   }
   
   constructCoulombAndExchangeMatrixInOrigIndices( theQmatWORK, theQmatACT );
   rotateOldToNew( theQmatACT );

}

void CheMPS2::CASSCF::buildTmatrix(){

   for ( int irrep = 0; irrep < num_irreps; irrep++ ){
      const int NumORB = iHandler->getNORB(irrep);
      for (int row = 0; row < NumORB; row++){
         const int HamIndexRow = iHandler->getOrigNOCCstart(irrep) + row;
         for (int col = 0; col < NumORB; col++){
            const int HamIndexCol = iHandler->getOrigNOCCstart(irrep) + col;
            theTmatrix->set( irrep, row, col, HamOrig->getTmat(HamIndexRow, HamIndexCol) );
         }
      }
   }
   
   rotateOldToNew( theTmatrix );

}

void CheMPS2::CASSCF::fillConstAndTmatDMRG(Hamiltonian * HamDMRG) const{

   //Constant part of the energy
   double value = HamOrig->getEconst();
   for ( int irrep = 0; irrep < num_irreps; irrep++ ){
      for (int orb = 0; orb < iHandler->getNOCC(irrep); orb++){
         value += 2 * theTmatrix->get(irrep, orb, orb) + theQmatOCC->get(irrep, orb, orb);
      }
   }
   HamDMRG->setEconst(value);
   
   //One-body terms: diagonal in the irreps
   for ( int irrep = 0; irrep < num_irreps; irrep++ ){
      const int passedDMRG  = iHandler->getDMRGcumulative(irrep);
      const int linsizeDMRG = iHandler->getNDMRG(irrep);
      const int NumOCC      = iHandler->getNOCC(irrep);
      for (int cnt1=0; cnt1<linsizeDMRG; cnt1++){
         for (int cnt2=cnt1; cnt2<linsizeDMRG; cnt2++){
            HamDMRG->setTmat( passedDMRG+cnt1, passedDMRG+cnt2, theTmatrix->get(irrep, NumOCC+cnt1, NumOCC+cnt2) + theQmatOCC->get(irrep, NumOCC+cnt1, NumOCC+cnt2) );
         }
      }
   }

}

void CheMPS2::CASSCF::buildKmatAndersson( DMRGSCFmatrix * result ){

   theQmatWORK->clear();

   // Construct ( 2 - RDM ) * RDM in the original orbitals in theQmatWORK
   for ( int irrep = 0; irrep < num_irreps; irrep++ ){

      int NACT = iHandler->getNDMRG( irrep );
      if ( NACT > 0 ){

         // In the current orbitals, ( 2 - RDM ) * RDM differs only from zero for the active space
         int NACT = iHandler->getNDMRG( irrep );
         const int JUMP = iHandler->getDMRGcumulative( irrep );
         double * data  = theQmatWORK->getBlock( irrep );
         for ( int row = 0; row < NACT; row++ ){
            for ( int col = 0; col < NACT; col++ ){
               double value = 2 * DMRG1DM[ JUMP + row + nOrbDMRG * ( JUMP + col ) ];
               for ( int sum = 0; sum < NACT; sum++ ){
                  value -= DMRG1DM[ JUMP + row + nOrbDMRG * ( JUMP + sum ) ] * DMRG1DM[ JUMP + sum + nOrbDMRG * ( JUMP + col ) ];
               }
               data[ row + NACT * col ] = value;
            }
         }

         // Rotate ( 2 - RDM ) * RDM from the current to the original orbitals
         int NOCC = iHandler->getNOCC( irrep );
         int NORB = iHandler->getNORB( irrep );
         double alpha  = 1.0;
         double beta   = 0.0;
         double * Umat = unitary->getBlock( irrep ) + NOCC;
         double * work =  result->getBlock( irrep );
         char trans    = 'T';
         char notrans  = 'N';
         dgemm_( &trans,   &notrans, &NORB, &NACT, &NACT, &alpha, Umat, &NORB, data, &NACT, &beta, work, &NORB );
         dgemm_( &notrans, &notrans, &NORB, &NORB, &NACT, &alpha, work, &NORB, Umat, &NORB, &beta, data, &NORB );

      }
   }

   // Construct Andersson's K-matrix from Theor. Chim. Acta 91, 31-46 (1995).
   for ( int irrep1 = 0; irrep1 < num_irreps; irrep1++ ){
      const int shift1 = iHandler->getOrigNOCCstart( irrep1 );
      const int NORB1  = iHandler->getNORB( irrep1 );
      for ( int row1 = 0; row1 < NORB1; row1++ ){
         for ( int col1 = 0; col1 < NORB1; col1++ ){
            double value = 0.0;
            for ( int irrep2 = 0; irrep2 < num_irreps; irrep2++ ){
               const int shift2 = iHandler->getOrigNOCCstart( irrep2 );
               const int NORB2  = iHandler->getNORB( irrep2 );
               for ( int row2 = 0; row2 < NORB2; row2++ ){
                  for ( int col2 = 0; col2 < NORB2; col2++ ){
                     value += theQmatWORK->get( irrep2, row2, col2 ) * HamOrig->getVmat( shift1 + row1, shift2 + col2, shift2 + row2, shift1 + col1 );
                  }
               }
            }
            result->set( irrep1, row1, col1, value );
         }
      }
   }

   rotateOldToNew( result );

}

void CheMPS2::CASSCF::pseudocanonical_occupied( const DMRGSCFmatrix * Tmat, const DMRGSCFmatrix * Qocc, const DMRGSCFmatrix * Qact, DMRGSCFunitary * Umat, double * work1, double * work2, const DMRGSCFindices * idx ){

   const int n_irreps = idx->getNirreps();
   for ( int irrep = 0; irrep < n_irreps; irrep++ ){

      int NORB = idx->getNORB( irrep );
      int NOCC = idx->getNOCC( irrep );
      if ( NOCC > 1 ){

         // Construct the occupied-occupied block of the Fock matrix
         for ( int row = 0; row < NOCC; row++ ){
            for ( int col = 0; col < NOCC; col++ ){
               work1[ row + NOCC * col ] = ( Tmat->get( irrep, row, col )
                                           + Qocc->get( irrep, row, col )
                                           + Qact->get( irrep, row, col ) );
            }
         }

         // Diagonalize the occupied-occupied block
         char jobz = 'V';
         char uplo = 'U';
         int info;
         int size = max( 3 * NOCC - 1, NOCC * NOCC );
         dsyev_( &jobz, &uplo, &NOCC, work1, &NOCC, work2 + size, work2, &size, &info );

         // Adjust the u-matrix accordingly
         double * umatrix = Umat->getBlock( irrep );
         for ( int row = 0; row < NOCC; row++ ){
            for ( int col = 0; col < NORB; col++ ){
               work2[ row + NOCC * col ] = umatrix[ row + NORB * col ];
            }
         }
         char trans   = 'T';
         char notrans = 'N';
         double one = 1.0;
         double set = 0.0;
         dgemm_( &trans, &notrans, &NOCC, &NORB, &NOCC, &one, work1, &NOCC, work2, &NOCC, &set, umatrix, &NORB );

      }
   }

}

void CheMPS2::CASSCF::pseudocanonical_virtual( const DMRGSCFmatrix * Tmat, const DMRGSCFmatrix * Qocc, const DMRGSCFmatrix * Qact, DMRGSCFunitary * Umat, double * work1, double * work2, const DMRGSCFindices * idx ){

   const int n_irreps = idx->getNirreps();
   for ( int irrep = 0; irrep < n_irreps; irrep++ ){

      int NORB  = idx->getNORB( irrep );
      int NVIRT = idx->getNVIRT( irrep );
      int NJUMP = NORB - NVIRT;
      if ( NVIRT > 1 ){

         // Construct the virtual-virtual block of the Fock matrix
         for ( int row = 0; row < NVIRT; row++ ){
            for ( int col = 0; col < NVIRT; col++ ){
               work1[ row + NVIRT * col ] = ( Tmat->get( irrep, NJUMP + row, NJUMP + col )
                                            + Qocc->get( irrep, NJUMP + row, NJUMP + col )
                                            + Qact->get( irrep, NJUMP + row, NJUMP + col ) );
            }
         }

         // Diagonalize the virtual-virtual block
         char jobz = 'V';
         char uplo = 'U';
         int info;
         int size = max( 3 * NVIRT - 1, NVIRT * NVIRT );
         dsyev_( &jobz, &uplo, &NVIRT, work1, &NVIRT, work2 + size, work2, &size, &info );

         // Adjust the u-matrix accordingly
         double * umatrix = Umat->getBlock( irrep ) + NJUMP;
         for ( int row = 0; row < NVIRT; row++ ){
            for ( int col = 0; col < NORB; col++ ){
               work2[ row + NVIRT * col ] = umatrix[ row + NORB * col ];
            }
         }
         char trans   = 'T';
         char notrans = 'N';
         double one = 1.0;
         double set = 0.0;
         dgemm_( &trans, &notrans, &NVIRT, &NORB, &NVIRT, &one, work1, &NVIRT, work2, &NVIRT, &set, umatrix, &NORB );

      }
   }

}


