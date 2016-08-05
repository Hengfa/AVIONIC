#include "agile/agile.hpp"
#include "agile/io/file.hpp"
#include "agile/gpu_timer.hpp"
#include "agile/io/dicom.hpp"
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <fstream>
#include <iostream>
#include <math.h>
#include <algorithm> 
#include "../include/raw_data_preparation.h"
#include "../include/cartesian_coil_construction.h"
#include "../include/noncartesian_coil_construction.h"
#include "../include/ictgv2.h"
#include "../include/tv.h"
#include "../include/noncartesian_operator.h"
#include "../include/cartesian_operator3d.h"
#include "../include/options_parser.h"
#include "../include/utils.h"
template <typename TType>

bool LoadGPUVectorFromFile(std::string &filename, TType &data)
{
  std::string extension = utils::GetFileExtension(filename);
  std::cout << "trying to load data from: " << filename << " | Extension: " << extension << std::endl;

  std::vector<typename TType::value_type> dataHost;  
  if (extension.compare(".bin") == 0)
  {
   if (!agile::readVectorFile(filename.c_str(), dataHost))
    {
      return false;
    }
    data.assignFromHost(dataHost.begin(), dataHost.end());
    std::cout << "norm vector: " << agile::norm1(data) << std::endl;
  }
  else if (extension.compare(".cfl") == 0)
  {
    long dimensions[4];//Dimension dimsread; // = op.dims;
    //for (int i=0; i < 4; i++)
    //   dimensions[i]=1;

    utils::ReadCflHeader(filename, dimensions);
    
    unsigned N = dimensions[0];
    for (int i=1; i < 4; i++)
      N=N*dimensions[i];

    //unsigned N = dimsread.width*dimsread.height*dimsread.depth*dimsread.coils;
    std::cout << "N= " << N << std::endl;
    //if (!agile::readCflFile(filename.c_str(), N, dataHost))
    //{
    //  return false;
    //}
    //data.assignFromHost(dataHost.begin(), dataHost.end());
    //std::cout << "norm vector: " << agile::norm1(data) << std::endl;
    return false;
  }
  else
    return false;
return true;
}

void GenerateReconOperator(PDRecon **recon, OptionsParser &options,
                           BaseOperator *mrOp)
{
  Dimension dims = options.dims;
  assert(dims.width == 0 || dims.height == 0 || dims.depth ==0 ||dims.coils == 0 ||
         dims.frames == 0);

  // adapt dims to correspond to image space dimensions
  std::cout << "method set to: ";
  switch (options.method)
  {
  case TV:
  {
    std::cout << "TV" << std::endl;
    *recon = new class TV(dims.width, dims.height, dims.coils, dims.frames,
                          options.tvParams, mrOp);
    break;
  }
  case TGV2:
  {
    std::cout << "TGV2" << std::endl;
    *recon = new class TGV2(dims.width, dims.height, dims.coils, dims.frames,
                            options.tgv2Params, mrOp);
    break;
  }
  case TGV2_3D:
  {
    std::cout << "TGV2_3D" << std::endl;
    *recon = new class TGV2_3D(dims.width, dims.height, dims.depth, dims.coils,
                            options.tgv2_3DParams, mrOp);
    break;
  }
  case ICTGV2:
  {
    std::cout << "ICTGV2" << std::endl;
    *recon = new class ICTGV2(dims.width, dims.height, dims.coils, dims.frames,
                              options.ictgv2Params, mrOp);
    break;
  }
  };

  (*recon)->SetVerbose(options.verbose);

  if (options.debugstep > 0)
  {
    (*recon)->SetDebug(true, options.debugstep);
  }
  else
  {
    (*recon)->SetDebug(false, options.debugstep);
  }
}

template <typename TType>
void ExportAdditionalResultsToMatlabBin(const char *outputDir,
                                        const char *filename,
                                        agile::GPUVector<TType> &result)
{
  std::vector<TType> resHost(result.size());
  result.copyToHost(resHost);
  std::string outputPath = boost::lexical_cast<std::string>(outputDir) + "/" +
                           boost::lexical_cast<std::string>(filename);
  agile::writeVectorFile(outputPath.c_str(), resHost);
}

void PerformCartesianCoilConstruction(Dimension &dims, OptionsParser &op,
                                      CVector &kdata, CVector &u, CVector &b1,
                                      RVector &mask, communicator_type &com)
{
  // Create MR Operator
  CartesianOperator *cartOp = new CartesianOperator(
      dims.width, dims.height, dims.coils, dims.frames, mask, false);
  CartesianCoilConstruction coilConstruction(
      dims.width, dims.height, dims.coils, dims.frames, op.coilParams, cartOp);
  coilConstruction.SetVerbose(op.verbose);

  coilConstruction.PerformCoilConstruction(kdata, u, b1, com);
  delete cartOp;
}

void PerformNonCartesianCoilConstruction(Dimension &dims, OptionsParser &op,
                                         CVector &kdata, CVector &u,
                                         CVector &b1, RVector &mask, RVector &w,
                                         communicator_type &com)
{
  unsigned nFE = dims.readouts;
  unsigned spokesPerFrame = dims.encodings;
  unsigned int nTraj = dims.frames * spokesPerFrame * nFE;

  // TODO check if density data is already generated by RAW data import
  // or if it has to be loaded from file
  if (w.size() == 0)
  {
    w = RVector(nTraj);
    if (!LoadGPUVectorFromFile(op.densityFilename, w))
    {
      std::cerr << "Density compensation data could not be loaded!"
                << std::endl;
      throw std::invalid_argument(
          "Density compensation data could not be loaded!");
    }
  }

  NoncartesianOperator *noncartOp = new NoncartesianOperator(
      dims.width, dims.height, dims.coils, dims.frames,
      spokesPerFrame * dims.frames, nFE, spokesPerFrame, mask, w);

  NoncartesianCoilConstruction nonCartCoilConstruction(
      dims.width, dims.height, dims.coils, dims.frames, op.coilParams,
      noncartOp);

  nonCartCoilConstruction.SetVerbose(op.verbose);
  nonCartCoilConstruction.PerformCoilConstruction(kdata, u, b1, com);
  delete noncartOp;
}

void PerformRawDataPreparation(Dimension &dims, OptionsParser &op,
                               CVector &kdata, RVector &mask, RVector &w)
{
  RawDataPreparation rdp(op, true, true, true, true);

  std::cout << "INFO: Loading RAW data from file/directory: "
            << op.kdataFilename << std::endl;
  std::string outputDir = utils::GetParentDirectory(op.outputFilename);

  rdp.PrepareRawData(kdata, mask, w, dims);

  // Extract dicom raw data and prepare it
  if (op.nonuniform)
  {
    ExportAdditionalResultsToMatlabBin(outputDir.c_str(), "w.bin", w);
  }

  ExportAdditionalResultsToMatlabBin(outputDir.c_str(), "mask.bin", mask);
  ExportAdditionalResultsToMatlabBin(outputDir.c_str(), "kdata.bin", kdata);
}

// ==================================================================================================================
// BEGIN: main
// ==================================================================================================================
int main(int argc, char *argv[])
{
  OptionsParser op;

  if (!op.ParseOptions(argc, argv))
    return -1;

  std::string outputDir = utils::GetParentDirectory(op.outputFilename);

  communicator_type com;
  com.allocateGPU();
  agile::GPUTimer timer;
  timer.start();

  agile::GPUEnvironment::printInformation(std::cout);
  std::cout << std::endl;

  // kdata
  CVector kdata;
  // kspace mask/trajectory
  RVector mask;

  // density compensation in case of nonuniform data
  RVector w(0);

  // get data dimensions
  std::string extension = utils::GetFileExtension(op.kdataFilename);
  std::cout << "Extension:" << extension << std::endl;
  
  Dimension dims; // = op.dims;
  if (extension.compare(".bin") == 0)
    dims = op.dims;
  else if (extension.compare(".cfl") == 0)
  {
      long dimensions[4];
      utils::ReadCflHeader(op.kdataFilename, dimensions);
      dims.width=dimensions[0];
      dims.height=dimensions[1];
      dims.depth=dimensions[2];
 
      dims.readouts=dimensions[0];
      dims.encodings=dimensions[1];
      dims.encodings2=dimensions[2];

      dims.coils=dimensions[3];
      dims.frames=dimensions[3];
      std::cout << "DIMS main; nx: " << dims.width << " / ny:" << dims.height << " / nz:" << dims.depth << " / nc:" << dims.coils << std::endl;
      std::cout << "DIMS main; nRO: " << dims.readouts << " / nENC1:" << dims.encodings << " / nENC2:" << dims.encodings2 << " / nframes:" << dims.frames << std::endl;
  }

  if (op.rawdata)
  {
    PerformRawDataPreparation(dims, op, kdata, mask, w);
  }
  else
  {
    std::cout << "Binary files defined...." << std::endl;
    if (!LoadGPUVectorFromFile(op.kdataFilename, kdata))
      return -1;
    else
      std::cout << "Data File " << op.kdataFilename << " successfully loaded." << std::endl;
 
    if (!LoadGPUVectorFromFile(op.maskFilename, mask))
      return -1;
    else
      std::cout << "Mask File " << op.maskFilename  << " successfully loaded." << std::endl;
    
  
    // set values in data-array to zero according to mask
    if (!op.nonuniform)
    { 
      if (op.method==TGV2_3D)
      {
        for (unsigned coil = 0; coil < dims.coils; coil++)
        {
          unsigned offset = dims.width * dims.height * dims.depth * coil;
           agile::lowlevel::multiplyElementwise(
              kdata.data() + offset, mask.data(),
              kdata.data() + offset, dims.width * dims.height * dims.depth);
        }
      }
      else
      {
        for (unsigned frame = 0; frame < dims.frames; frame++)
        {
          unsigned offset = dims.width * dims.height * dims.coils * frame;
          for (unsigned coil = 0; coil < dims.coils; coil++)
            {
            unsigned int x_offset = offset + coil * dims.width * dims.height;
            agile::lowlevel::multiplyElementwise(
              kdata.data() + x_offset, mask.data() + dims.width * dims.height * frame,
              kdata.data() + x_offset, dims.width * dims.height);
            }
        }
      }
    }
  }

  BaseOperator *baseOp = NULL;

  unsigned N;
  if (op.method==TGV2_3D)
    N = dims.width * dims.height * dims.depth;
  else 
    N = dims.width * dims.height;

  CVector b1, u0;
  // init b1 and u0
  b1 = CVector(N * dims.coils);
  b1.assign(N * dims.coils, 1.0);
  u0 = CVector(N);
  u0.assign(N, 0.0);

  // check if b1 and u0 data is provided
  if (LoadGPUVectorFromFile(op.sensitivitiesFilename, b1))
  {
    std::cout << "B1 File " << op.sensitivitiesFilename
              << " successfully loaded." << std::endl;
    
    if (LoadGPUVectorFromFile(op.u0Filename, u0))
    {
      std::cout << " initial solution (u0) file " << op.u0Filename
                << " successfully loaded." << std::endl;
    }
    else
    {
      std::cout << "no initial solution (u0) data provided!" << std::endl;
    }
  }
  else
  {
    
    if (op.method==TGV2_3D)
    {
       std::cerr << "Coil Construction for 3D reconstruction not implemented! Provide b1 seperately!"
                << std::endl;
       return -1;
    }
    
    std::cout << "Performing Coil Construction!" << std::endl;
    CVector u(N * dims.coils);
    u.assign(N * dims.coils, 0.0);

    if (op.nonuniform)
    {
      PerformNonCartesianCoilConstruction(dims, op, kdata, u, b1, mask, w, com);
    }
    else
    {
      PerformCartesianCoilConstruction(dims, op, kdata, u, b1, mask, com);
    }
    utils::GetSubVector(u, u0, dims.coils - 1, N);

    ExportAdditionalResultsToMatlabBin(outputDir.c_str(),
                                       "b1_reconstructed.bin", b1);
    ExportAdditionalResultsToMatlabBin(outputDir.c_str(),
                                       "u0_reconstructed.bin", u0);
  }

  if (op.nonuniform)
  {
    unsigned nFE = dims.readouts;
    unsigned spokesPerFrame = dims.encodings;

    std::cout << "Init NonCartesian Operator using kernelWidth:"
              << op.gpuNUFFTParams.kernelWidth
              << " sectorWidth:" << op.gpuNUFFTParams.sectorWidth
              << " OSF:" << op.gpuNUFFTParams.osf << std::endl;

    // Create 2d-t MR Operator
    baseOp = new NoncartesianOperator(
        dims.width, dims.height, dims.coils, dims.frames,
        spokesPerFrame * dims.frames, nFE, spokesPerFrame, mask, w, b1,
        op.gpuNUFFTParams.kernelWidth, op.gpuNUFFTParams.sectorWidth,
        op.gpuNUFFTParams.osf);
  }
  else
  {
    // Create 3d MR Operator
    if (op.method==TGV2_3D)
    {
      baseOp = new CartesianOperator3D(dims.width, dims.height, dims.depth,
                                   dims.coils, mask, false);
    }
    else
    {
      baseOp = new CartesianOperator(dims.width, dims.height, dims.coils,
                                   dims.frames, mask, false);
    }
  }

  // ==================================================================================================================
  // BEGIN: Perform iterative (TV, TGV2, TGV_3D, ICTGV2) reconstruction
  // ==================================================================================================================
  PDRecon *recon = NULL;
  GenerateReconOperator(&recon, op, baseOp);

  CVector x(0); // resize at runtime
  if (op.method==TGV2_3D)
  {
    x.resize(N, 0.0);
    agile::copy(u0, x);
  }
  else
  {
    x.resize(N * dims.frames, 0.0);
    for (unsigned frame = 0; frame < dims.frames; frame++)
    {
      utils::SetSubVector(u0, x, frame, N);
    }
  }
 
  std::cout << "Initialization time: " << timer.stop() / 1000 << "s"
            << std::endl;

  timer.start();

  // run reconstruction
  recon->IterativeReconstruction(kdata, x, b1);
  std::cout << "Execution time: " << timer.stop() / 1000 << "s" << std::endl; 
  // ==================================================================================================================
  // END: Perform iterative (TV, TGV2, TGV_3D, ICTGV2) reconstruction
  // ==================================================================================================================


  // ==================================================================================================================
  // BEGIN: Define output 
  // ================================================================================================================== 
    std::string extension_out = utils::GetFileExtension(op.outputFilename);
    std::vector<CType> xHost;
    x.copyToHost(xHost);
 
  // write reconstruction to bin file 
  if (extension_out.compare(".bin") == 0)
  {
    std::cout << "writing output file to: " << op.outputFilename << std::endl;
    agile::writeVectorFile(op.outputFilename.c_str(), xHost); 
  }
  // write reconstruction to h5 file 
  else if (extension_out.compare(".h5") == 0)
  {
    std::vector<size_t> dimVec;
    dimVec.push_back(dims.width);
    dimVec.push_back(dims.height);
    dimVec.push_back(dims.frames);
    utils::WriteH5File(op.outputFilename, "recon", dimVec, xHost);
  }
  // write reconstruction to dicom file 
  else if (extension_out.compare(".dcm") == 0)   
  {
    agile::DICOM dicomfile;
    std::string filenamewoe = utils::GetFilename(op.outputFilename);
    std::ostringstream ss;	 
    for (unsigned frame = 0; frame < dims.frames; frame++) 
    {
      std::vector<float> xoutmag;
      std::vector<float> xoutphs;
      for( unsigned i = N*frame; i < N*(frame+1); i++ )
        {
        xoutmag.push_back( (float)std::sqrt( pow(real(xHost[i]),2) + pow(std::imag(xHost[i]),2) ) ) ;
        xoutphs.push_back( (float)std::atan2( real(xHost[i]),std::imag(xHost[i]) ) );
        }
      
      ss << std::setw(3) << std::setfill('0') << frame;
      // magnitude
      const std::string str = ss.str();      
      std::string outputPath1 = boost::lexical_cast<std::string>(outputDir) + "/" + filenamewoe + "_magframe" + ss.str() + ".dcm"; 
      std::cout << "writing dicom file to: " << outputPath1 << std::endl;   
      //dicomfile.set_dicominfo(_in_dicomfile.get_dicominfo());  
      dicomfile.gendicom(outputPath1.c_str(), xoutmag, dims.height, dims.width); 
 
      // phase     
      std::string outputPath2 = boost::lexical_cast<std::string>(outputDir) + "/" + filenamewoe + "_phsframe" + ss.str() + ".dcm"; 
      std::cout << "writing dicom file to: " << outputPath2 << std::endl; 
  
      //dicomfile.set_dicominfo(_in_dicomfile.get_dicominfo());  
      dicomfile.gendicom(outputPath2.c_str(), xoutphs, dims.height, dims.width); 
 
      ss.str("");   
    }
  }
  else
  {
     // write reconstruction to binary file
     agile::writeVectorFile(op.outputFilename.c_str(), xHost);
  }

  // export additional information (pdgap, ictgv-component)
  if (op.extradata)
    recon->ExportAdditionalResults(outputDir.c_str(),
                                   &ExportAdditionalResultsToMatlabBin);
  // ==================================================================================================================
  // END: Define output 
  // ================================================================================================================== 

  delete recon;
  delete baseOp;
}

