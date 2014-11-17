/****************************************************************************
**
** Copyright (C) 2014 Klaralvdalens Datakonsult AB (KDAB).
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt3D module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QT3D_RENDER_RENDERNODE_H
#define QT3D_RENDER_RENDERNODE_H

#include <Qt3DRenderer/private/renderer_p.h>
#include <Qt3DRenderer/private/handle_types_p.h>
#include <Qt3DCore/qbackendnode.h>
#include <Qt3DCore/qnodeid.h>
#include <Qt3DCore/qhandle.h>
#include <QVector>

QT_BEGIN_NAMESPACE

class QMatrix4x4;

namespace Qt3D {

class QRenderAspect;
class Sphere;
class QNode;
class QEntity;
class QComponent;

namespace Render {

class Renderer;

class RenderEntity : public QBackendNode
{
public:
    RenderEntity();
    ~RenderEntity();
    void cleanup();

    void setParentHandle(HEntity parentHandle);
    void setRenderer(Renderer *renderer);
    void sceneChangeEvent(const QSceneChangePtr &e) Q_DECL_OVERRIDE;
    void updateFromPeer(QNode *peer) Q_DECL_OVERRIDE;

    void dump() const;

    void  setHandle(HEntity handle);
    HEntity handle() const { return m_handle; }
    RenderEntity *parent() const;
    HEntity parentHandle() const { return m_parentHandle; }

    void appendChildHandle(HEntity childHandle);
    void removeChildHandle(HEntity childHandle);
    QVector<HEntity> childrenHandles() const { return m_childrenHandles; }
    QVector<RenderEntity *> children() const;

    QMatrix4x4 *worldTransform();
    const QMatrix4x4 *worldTransform() const;
    Sphere *localBoundingVolume() { return m_localBoundingVolume; }
    Sphere *worldBoundingVolume() { return m_worldBoundingVolume; }

    void addComponent(QComponent *component);
    void removeComponent(QComponent *component);

    template<class Backend, uint INDEXBITS>
    QHandle<Backend, INDEXBITS> componentHandle() const
    {
        return QHandle<Backend, INDEXBITS>();
    }

    template<class Backend, uint INDEXBITS>
    QList<QHandle<Backend, INDEXBITS> > componentsHandle() const
    {
        return QList<QHandle<Backend, INDEXBITS> >();
    }

    template<class Backend>
    Backend *renderComponent() const
    {
        return Q_NULLPTR;
    }

    template<class Backend>
    QList<Backend *> renderComponents() const
    {
        return QList<Backend *>();
    }

    template<class Backend>
    QNodeId componentUuid() const
    {
        return QNodeId();
    }

    template<class Backend>
    QList<QNodeId> componentsUuid() const
    {
        return QList<QNodeId>();
    }

private:

    template<class Frontend, class Backend, class Manager>
    void createRenderComponentHelper(Frontend *frontend, Manager *manager)
    {
        // We index using the Frontend uuid
        if (!manager->contains(frontend->uuid())) {
            Backend *backend = manager->getOrCreateResource(frontend->uuid());
            backend->setRenderer(m_renderer);
            backend->setPeer(frontend);
        }
    }

    Renderer *m_renderer;
    HEntity m_handle;
    HEntity m_parentHandle;
    QVector<HEntity > m_childrenHandles;

    HMatrix m_worldTransform;
    Sphere *m_localBoundingVolume;
    Sphere *m_worldBoundingVolume;

    // Handles to Components
    QNodeId m_transformComponent;
    QNodeId m_meshComponent;
    QNodeId m_materialComponent;
    QNodeId m_cameraComponent;
    QList<QNodeId> m_layerComponents;
    QList<QNodeId> m_lightComponents;
    QList<QNodeId> m_shaderDataComponents;

    QString m_objectName;
};

template<>
HMesh RenderEntity::componentHandle<RenderMesh>() const;

template<>
RenderMesh *RenderEntity::renderComponent<RenderMesh>() const;

template<>
HMaterial RenderEntity::componentHandle<RenderMaterial>() const;

template<>
RenderMaterial *RenderEntity::renderComponent<RenderMaterial>() const;

template<>
HCamera RenderEntity::componentHandle<RenderCameraLens>() const;

template<>
RenderCameraLens *RenderEntity::renderComponent<RenderCameraLens>() const;

template<>
HTransform RenderEntity::componentHandle<RenderTransform>() const;

template<>
RenderTransform *RenderEntity::renderComponent<RenderTransform>() const;

template<>
QNodeId RenderEntity::componentUuid<RenderTransform>() const;

template<>
QNodeId RenderEntity::componentUuid<RenderCameraLens>() const;

template<>
QNodeId RenderEntity::componentUuid<RenderMaterial>() const;

template<>
QNodeId RenderEntity::componentUuid<RenderMesh>() const;

template<>
QList<HLayer> RenderEntity::componentsHandle<RenderLayer>() const;

template<>
QList<RenderLayer *> RenderEntity::renderComponents<RenderLayer>() const;

template<>
QList<QNodeId> RenderEntity::componentsUuid<RenderLayer>() const;

template<>
QList<HLight> RenderEntity::componentsHandle<RenderLight>() const;

template<>
QList<RenderLight *> RenderEntity::renderComponents<RenderLight>() const;

template<>
QList<QNodeId> RenderEntity::componentsUuid<RenderLight>() const;

template<>
QList<HShaderData> RenderEntity::componentsHandle<RenderShaderData>() const;

template<>
QList<RenderShaderData *> RenderEntity::renderComponents<RenderShaderData>() const;

template<>
QList<QNodeId> RenderEntity::componentsUuid<RenderShaderData>() const;

class RenderEntityFunctor : public QBackendNodeFunctor
{
public:
    explicit RenderEntityFunctor(Renderer *renderer);
    QBackendNode *create(QNode *frontend) const Q_DECL_OVERRIDE;
    QBackendNode *get(QNode *frontend) const Q_DECL_OVERRIDE;
    void destroy(QNode *frontend) const Q_DECL_OVERRIDE;

private:
    Renderer *m_renderer;
};

} // namespace Render
} // namespace Qt3D

QT_END_NAMESPACE

#endif // QT3D_RENDER_RENDERNODE_H
