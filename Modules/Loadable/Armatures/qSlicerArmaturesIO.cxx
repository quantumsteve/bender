/*=========================================================================

  Program: Bender

  Copyright (c) Kitware Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

=========================================================================*/

// Qt includes
#include <QDebug>
#include <QFileInfo>

// SlicerQt includes
#include "qSlicerArmaturesIO.h"
#include "qSlicerArmaturesIOOptionsWidget.h"

// Logic includes
#include "vtkSlicerArmaturesLogic.h"
#include "vtkSlicerModelsLogic.h"

// Bender includes
#include "vtkBVHReader.h"

// MRML includes
#include "vtkMRMLArmatureNode.h"
#include "vtkMRMLArmatureStorageNode.h"
#include "vtkMRMLBoneNode.h"
#include "vtkMRMLModelNode.h"

// VTK includes
#include <vtkNew.h>
#include <vtkSmartPointer.h>

//-----------------------------------------------------------------------------
class qSlicerArmaturesIOPrivate
{
public:
  vtkSmartPointer<vtkSlicerArmaturesLogic> ArmaturesLogic;
};

//-----------------------------------------------------------------------------
/// \ingroup Slicer_QtModules_Armatures
qSlicerArmaturesIO::qSlicerArmaturesIO(vtkSlicerArmaturesLogic* armaturesLogic, QObject* _parent)
  : Superclass(_parent)
  , d_ptr(new qSlicerArmaturesIOPrivate)
{
  this->setArmaturesLogic(armaturesLogic);
}

//-----------------------------------------------------------------------------
qSlicerArmaturesIO::~qSlicerArmaturesIO()
{
}

//-----------------------------------------------------------------------------
void qSlicerArmaturesIO::setArmaturesLogic(vtkSlicerArmaturesLogic* newArmaturesLogic)
{
  Q_D(qSlicerArmaturesIO);
  d->ArmaturesLogic = newArmaturesLogic;
}

//-----------------------------------------------------------------------------
vtkSlicerArmaturesLogic* qSlicerArmaturesIO::armaturesLogic()const
{
  Q_D(const qSlicerArmaturesIO);
  return d->ArmaturesLogic;
}

//-----------------------------------------------------------------------------
QString qSlicerArmaturesIO::description()const
{
  return "Armature";
}

//-----------------------------------------------------------------------------
qSlicerIO::IOFileType qSlicerArmaturesIO::fileType()const
{
  return "ArmatureFile";
}

//-----------------------------------------------------------------------------
QStringList qSlicerArmaturesIO::extensions()const
{
  return QStringList()
    << "Armature (*.arm *.vtk *.bvh)";
}

//-----------------------------------------------------------------------------
bool qSlicerArmaturesIO::load(const IOProperties& properties)
{
  Q_D(qSlicerArmaturesIO);
  Q_ASSERT(properties.contains("fileName"));
  QString fileName = properties["fileName"].toString();

  this->setLoadedNodes(QStringList());
  if (d->ArmaturesLogic.GetPointer() == 0)
    {
    return false;
    }

  if (properties.contains("targetArmature") && fileName.endsWith(".bvh"))
    {
    return this->importAnimationFromFile(properties);
    }
  else if (fileName.endsWith(".vtk")
    || fileName.endsWith(".arm")
    || fileName.endsWith(".bvh"))
    {
    vtkMRMLArmatureNode* armatureNode =
      d->ArmaturesLogic->AddArmatureFile(fileName.toLatin1());
    if (!armatureNode)
      {
      return false;
      }

    if (properties.contains("name"))
      {
      std::string uname = this->mrmlScene()->GetUniqueNameByString(
        properties["name"].toString().toLatin1());
      armatureNode->SetName(uname.c_str());
      }

    if (properties.contains("frame"))
      {
      armatureNode->SetFrame(properties["frame"].toUInt());
      }

    this->setLoadedNodes(QStringList(armatureNode->GetID()));
    return true;
    }

  return false;
}

//-----------------------------------------------------------------------------
bool qSlicerArmaturesIO::importAnimationFromFile(const IOProperties& properties)
{
  Q_D(qSlicerArmaturesIO);
  QString fileName = properties["fileName"].toString();
  QString currentArmatureID = properties["targetArmature"].toString();

  vtkMRMLArmatureNode* targetNode =
    vtkMRMLArmatureNode::SafeDownCast(
      d->ArmaturesLogic->GetMRMLScene()->GetNodeByID(
        currentArmatureID.toLatin1()));
  if (!targetNode)
    {
    qCritical()<<"Could not find target node. Animation import failed.";
    return false;
    }

  vtkNew<vtkBVHReader> reader;
  reader->SetFileName(fileName.toLatin1());
  reader->Update();
  if (!reader->GetArmature())
    {
    qCritical()<<"Could not read in animation file: "<<fileName
      <<". Make sure the file is valid. Animation import failed.";
    return false;
    };
  reader->SetFrame(properties["frame"].toUInt());
  reader->GetArmature()->SetWidgetState(vtkArmatureWidget::Pose);


  CorrespondenceMap correspondence;
  bool foundCorrespondence =
    this->getCorrespondenceMap(targetNode, reader->GetArmature(), correspondence);
  if (!foundCorrespondence)
    {
    qCritical()<<"Could find a correspondence between the target armature and"
      <<" the animated armature. Animation import failed.";
    return false;
    }

  targetNode->ResetPoseMode();
  int oldState = targetNode->GetWidgetState();
  targetNode->SetWidgetState(vtkMRMLArmatureNode::Pose);

  int i = 0;
  for (CorrespondenceMap::iterator it = correspondence.begin();
    it != correspondence.end(); ++it)
    {
    vtkMRMLBoneNode* boneNode = it->first;
    vtkBoneWidget* bone = it->second;
    assert(boneNode);
    assert(bone);

    // We have the rotation on the animation bone and we need to set it to
    // the target bone.
    vtkQuaterniond animation = bone->GetRestToPoseRotation();

    // First, we make the animation in the world basis.
    vtkQuaterniond animatedWorldToParent = bone->GetWorldToParentRestRotation();
    vtkQuaterniond worldAnimation =
      animatedWorldToParent.Inverse() * animation * animatedWorldToParent;

    // Then we change it to target basis
    vtkQuaterniond targetWorldToParent = boneNode->GetWorldToParentRestRotation();
    vtkQuaterniond targetAnimation =
      targetWorldToParent * worldAnimation * targetWorldToParent.Inverse();
    targetAnimation.Normalize();

    double axis[3];
    double angle = targetAnimation.GetRotationAngleAndAxis(axis);
    boneNode->RotateTailWithParentWXYZ(angle, axis);
    }

  targetNode->SetWidgetState(oldState);

  // Kill any potential animation if the armature was a bvh
  vtkMRMLArmatureStorageNode* storageNode =
    targetNode->GetArmatureStorageNode();
  if (storageNode)
    {
    targetNode->SetArmatureStorageNode(0);
    d->ArmaturesLogic->GetMRMLScene()->RemoveNode(storageNode);
    }

  return true;
}

//-----------------------------------------------------------------------------
bool qSlicerArmaturesIO
::getCorrespondenceMap(vtkMRMLArmatureNode* armatureNode,
                       vtkArmatureWidget* armature,
                       CorrespondenceMap& correspondence)
{
  // As for now, use simple name correspondence
  if (!armature || !armatureNode)
    {
    return false;
    }

  vtkNew<vtkCollection> bones;
  armatureNode->GetAllBones(bones.GetPointer());

 for (int i = 0; i < bones->GetNumberOfItems(); ++i)
    {
    vtkMRMLBoneNode* boneNode =
      vtkMRMLBoneNode::SafeDownCast(bones->GetItemAsObject(i));
    assert(boneNode);

    vtkBoneWidget* bone =
      armature->GetBoneByName(boneNode->GetName());

    if (!bone)
      {
      qWarning()<<"Could not find the bone name: "<<boneNode->GetName();
      return false;
      }

    correspondence.push_back(CorrespondencePair(boneNode, bone));
    }
  return true;
}

//-----------------------------------------------------------------------------
qSlicerIOOptions* qSlicerArmaturesIO::options()const
{
  return new qSlicerArmaturesIOOptionsWidget;
}
