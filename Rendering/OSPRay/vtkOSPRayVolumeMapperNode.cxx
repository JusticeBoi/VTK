/*=========================================================================

   Program:   Visualization Toolkit
   Module:    vtkOSPRayVolumeMapperNode.cxx

   Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
   All rights reserved.
   See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

      This software is distributed WITHOUT ANY WARRANTY; without even
      the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
      PURPOSE.  See the above copyright notice for more information.

 =========================================================================*/
#include "vtkOSPRayVolumeMapperNode.h"

#include "vtkAbstractVolumeMapper.h"
#include "vtkCellData.h"
#include "vtkColorTransferFunction.h"
#include "vtkDataArray.h"
#include "vtkInformation.h"
#include "vtkImageData.h"
#include "vtkObjectFactory.h"
#include "vtkOSPRayVolumeCache.h"
#include "vtkOSPRayRendererNode.h"
#include "vtkPiecewiseFunction.h"
#include "vtkPointData.h"
#include "vtkVolume.h"
#include "vtkVolumeNode.h"
#include "vtkVolumeProperty.h"
#include "vtkRenderer.h"

#include <algorithm>

#include "ospray/ospray.h"

//============================================================================
vtkStandardNewMacro(vtkOSPRayVolumeMapperNode);

//----------------------------------------------------------------------------
vtkOSPRayVolumeMapperNode::vtkOSPRayVolumeMapperNode()
{
  this->SamplingRate=0.0;
  this->NumColors = 128;
  this->OSPRayVolume = nullptr;
  this->TransferFunction = nullptr;
  this->Cache = new vtkOSPRayVolumeCache;
}

//----------------------------------------------------------------------------
vtkOSPRayVolumeMapperNode::~vtkOSPRayVolumeMapperNode()
{
  ospRelease(this->TransferFunction);
  if (this->OSPRayVolume && this->Cache->GetSize() == 0)
  {
    delete this->OSPRayVolume;
  }
  delete this->Cache;
}

//----------------------------------------------------------------------------
void vtkOSPRayVolumeMapperNode::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//----------------------------------------------------------------------------
void vtkOSPRayVolumeMapperNode::Render(bool prepass)
{
  if (prepass)
  {
    vtkVolumeNode* volNode = vtkVolumeNode::SafeDownCast(this->Parent);
    vtkVolume* vol = vtkVolume::SafeDownCast(volNode->GetRenderable());
    if (vol->GetVisibility() == false)
    {
      return;
    }
    vtkAbstractVolumeMapper* mapper = vtkAbstractVolumeMapper::SafeDownCast(this->GetRenderable());
    if (!vol->GetProperty())
    {
      // this is OK, happens in paraview client side for instance
      return;
    }

    vtkOSPRayRendererNode *orn =
      static_cast<vtkOSPRayRendererNode *>(
        this->GetFirstAncestorOfType("vtkOSPRayRendererNode"));
    vtkRenderer *ren = vtkRenderer::SafeDownCast(orn->GetRenderable());
    this->Cache->SetSize(vtkOSPRayRendererNode::GetTimeCacheSize(ren));

    osp::Model* OSPRayModel = orn->GetOModel();

    // make sure that we have scalar input and update the scalar input
    if ( mapper->GetDataSetInput() == nullptr )
    {
      //OK - PV cli/srv for instance vtkErrorMacro("VolumeMapper had no input!");
      return;
    }
    mapper->GetInputAlgorithm()->UpdateInformation();
    mapper->GetInputAlgorithm()->Update();

    vtkImageData *data = vtkImageData::SafeDownCast(mapper->GetDataSetInput());
    if (!data)
    {
      //vtkErrorMacro("VolumeMapper's Input has no data!");
      return;
    }

    int fieldAssociation;
    vtkDataArray *sa = vtkDataArray::SafeDownCast
      (this->GetArrayToProcess(data, fieldAssociation));
    if (!sa)
    {
      vtkErrorMacro("VolumeMapper's Input has no scalar array!");
      return;
    }

    if (!this->TransferFunction)
    {
      this->TransferFunction = ospNewTransferFunction("piecewise_linear");
    }

    vtkVolumeProperty* volProperty = vol->GetProperty();
    // when input data is modified
    vtkDataArray *sca = nullptr;
    if (mapper->GetDataSetInput()->GetMTime() > this->BuildTime)
    {
      double tstep = vtkOSPRayRendererNode::GetViewTime(ren);
      auto cached_Volume = this->Cache->GetFromCache(tstep);
      if (cached_Volume)
      {
        this->OSPRayVolume = cached_Volume;
      }
      else
      {
        int ncomp = sa->GetNumberOfComponents();
        if (ncomp>1)
        {
          int comp = 0;//mapper->GetArrayComponent(); not yet supported
          sca = sa->NewInstance();
          sca->SetNumberOfComponents(1);
          sca->SetNumberOfTuples(sa->GetNumberOfTuples());
          sca->CopyComponent(0, sa, comp);
          sa = sca;
        }
        int ScalarDataType = sa->GetDataType();
        void* ScalarDataPointer = sa->GetVoidPointer(0);
        int dim[3];
        data->GetDimensions(dim);
        if (fieldAssociation == vtkDataObject::FIELD_ASSOCIATION_CELLS)
        {
          dim[0] = dim[0]-1;
          dim[1] = dim[1]-1;
          dim[2] = dim[2]-1;
        }

        std::string voxelType;
        if (ScalarDataType == VTK_FLOAT)
        {
          voxelType = "float";
        }
        else if (ScalarDataType == VTK_UNSIGNED_CHAR)
        {
          voxelType = "uchar";
        }
        else if (ScalarDataType == VTK_UNSIGNED_SHORT)
        {
          voxelType = "ushort";
        }
        else if (ScalarDataType == VTK_SHORT)
        {
          voxelType = "short";
        }
        else if (ScalarDataType == VTK_DOUBLE)
        {
          voxelType = "double";
        }
        else
        {
          vtkErrorMacro("ERROR: Unsupported data type for ospray volumes, current supported data types are: float, uchar, short, ushort, and double.");
          return;
        }

        if (this->OSPRayVolume && this->Cache->GetSize() == 0)
        {
          delete this->OSPRayVolume;
        }
        this->OSPRayVolume = ospNewVolume("block_bricked_volume");
        this->Cache->AddToCache(tstep, this->OSPRayVolume);
        //
        // Send Volumetric data to OSPRay
        //
        ospSet3i(this->OSPRayVolume, "dimensions", dim[0], dim[1], dim[2]);
        double origin[3];
        double scale[3];
        data->GetOrigin(origin);
        vol->GetScale(scale);
        const double *bds = vol->GetBounds();
        origin[0] = bds[0];
        origin[1] = bds[2];
        origin[2] = bds[4];

        double spacing[3];
        data->GetSpacing(spacing);
        scale[0] = (bds[1]-bds[0])/double(dim[0]-1);
        scale[1] = (bds[3]-bds[2])/double(dim[1]-1);
        scale[2] = (bds[5]-bds[4])/double(dim[2]-1);

        ospSet3f(this->OSPRayVolume, "gridOrigin", origin[0], origin[1], origin[2]);
        ospSet3f(this->OSPRayVolume, "gridSpacing", scale[0], scale[1], scale[2]);
        ospSetString(this->OSPRayVolume, "voxelType", voxelType.c_str());

        osp::vec3i ll, uu;
        ll.x = 0, ll.y = 0, ll.z = 0;
        uu.x = dim[0], uu.y = dim[1], uu.z = dim[2];
        ospSetRegion(this->OSPRayVolume, ScalarDataPointer, ll, uu);

        ospSet2f(this->TransferFunction, "valueRange",
                 sa->GetRange()[0], sa->GetRange()[1]);
        ospSetObject(this->OSPRayVolume, "transferFunction",
                     this->TransferFunction);

        ospSet1f(OSPRayVolume, "adaptiveMaxSamplingRate", 1.2f);
        ospSet1f(OSPRayVolume, "adaptiveBacktrack", 0.01f);
        ospSet1i(OSPRayVolume, "adaptiveSampling", 1);
        if (this->SamplingRate == 0.0f)  // 0 means automatic sampling rate
        {
          //automatically determine sampling rate
          int minBound = std::min(std::min(dim[0],dim[1]),dim[2]);
          float minSamplingRate = 0.075f; // lower for min adaptive sampling step
          if (minBound < 100)
          {
            float s = (100.0f - minBound)/100.0f;
            ospSet1f(this->OSPRayVolume, "samplingRate", s*6.f + 1.f);
            ospSet1i(this->OSPRayVolume, "adaptiveSampling", 0); //turn off preIntegration
          }
          else if (minBound < 1000)
          {
            float s = std::min((900.0f - minBound)/1000.0f, 1.f);
            float s_new = (s*s*s*(0.5f-minSamplingRate) + minSamplingRate);
            ospSet1f(this->OSPRayVolume, "samplingRate", s_new);
            ospSet1f(this->OSPRayVolume, "adaptiveMaxSamplingRate", 2.f);
          }
          else
          {
            ospSet1f(this->OSPRayVolume, "samplingRate", minSamplingRate);
          }
        }
        else
        {
          ospSet1f(this->OSPRayVolume, "samplingRate", this->SamplingRate);
        }
        ospSet1f(this->OSPRayVolume, "adaptiveScalar", 15.f);
        ospSet1i(this->OSPRayVolume, "preIntegration", 0); //turn off preIntegration

        float rs = static_cast<float>(volProperty->GetSpecular(0)/16.); //16 chosen because near GL
        float gs = static_cast<float>(volProperty->GetSpecular(1)/16.);
        float bs = static_cast<float>(volProperty->GetSpecular(2)/16.);
        ospSet3f(this->OSPRayVolume, "specular", rs,gs,bs);

        ospSet1i(this->OSPRayVolume, "gradientShadingEnabled",
                 volProperty->GetShade());
        ospCommit(this->TransferFunction);
        ospCommit(this->OSPRayVolume);
      }
    }

    // test for modifications to volume properties
    if (vol->GetProperty()->GetMTime() > this->PropertyTime
        || mapper->GetDataSetInput()->GetMTime() > this->BuildTime)
    {
      vtkColorTransferFunction* colorTF =
        volProperty->GetRGBTransferFunction(0);
      vtkPiecewiseFunction *scalarTF = volProperty->GetScalarOpacity(0);

      this->TFVals.resize(this->NumColors*3);
      this->TFOVals.resize(this->NumColors);
      scalarTF->GetTable(sa->GetRange()[0],
                         sa->GetRange()[1],
                         this->NumColors,
                         &TFOVals[0]);
      colorTF->GetTable(sa->GetRange()[0],
                        sa->GetRange()[1],
                        this->NumColors,
                        &this->TFVals[0]);

      OSPData colorData = ospNewData(this->NumColors,
                                     OSP_FLOAT3,
                                     &this->TFVals[0]);
      ospSetData(this->TransferFunction, "colors", colorData);

      OSPData tfAlphaData = ospNewData(NumColors, OSP_FLOAT, &TFOVals[0]);
      ospSetData(this->TransferFunction, "opacities", tfAlphaData);

      this->PropertyTime.Modified();
      ospRelease(colorData);
      ospRelease(tfAlphaData);
      ospCommit(this->TransferFunction);
    }

    this->RenderTime = volNode->GetMTime();
    this->BuildTime.Modified();

    ospAddVolume(OSPRayModel, this->OSPRayVolume);

    if (sca)
    {
      sca->Delete();
    }
  }
}

//------------------------------------------------------------------------------
void vtkOSPRayVolumeMapperNode::UpdateTransferFunction(vtkVolume* vol)
{
  vtkVolumeProperty* volProperty = vol->GetProperty();
  vtkColorTransferFunction* colorTF =
    volProperty->GetRGBTransferFunction(0);
  vtkPiecewiseFunction *scalarTF = volProperty->GetScalarOpacity(0);
  double* tfRangeD = colorTF->GetRange();
  osp::vec2f tfRange = {float(tfRangeD[0]), float(tfRangeD[1])};

  this->TFVals.resize(this->NumColors*3);
  this->TFOVals.resize(this->NumColors);
  scalarTF->GetTable(tfRangeD[0],
                     tfRangeD[1],
                     this->NumColors,
                     &TFOVals[0]);
  colorTF->GetTable(tfRangeD[0],
                    tfRangeD[1],
                    this->NumColors,
                    &this->TFVals[0]);

  OSPData colorData = ospNewData(this->NumColors,
                                 OSP_FLOAT3,
                                 &this->TFVals[0]);
  ospSetData(this->TransferFunction, "colors", colorData);

  OSPData tfAlphaData = ospNewData(this->NumColors, OSP_FLOAT, &TFOVals[0]);
  ospSetData(this->TransferFunction, "opacities", tfAlphaData);
  ospSetVec2f(this->TransferFunction, "valueRange", tfRange);
  ospCommit(this->TransferFunction);
  ospSetObject(this->OSPRayVolume, "transferFunction",
               this->TransferFunction);

  ospRelease(colorData);
  ospRelease(tfAlphaData);
}
