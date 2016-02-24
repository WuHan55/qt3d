/****************************************************************************
**
** Copyright (C) 2014 Klaralvdalens Datakonsult AB (KDAB).
** Copyright (C) 2016 The Qt Company Ltd and/or its subsidiary(-ies).
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt3D module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "renderer_p.h"

#include <Qt3DCore/qentity.h>

#include <Qt3DRender/qmaterial.h>
#include <Qt3DRender/qmesh.h>
#include <Qt3DRender/qrenderpass.h>
#include <Qt3DRender/qshaderprogram.h>
#include <Qt3DRender/qtechnique.h>
#include <Qt3DRender/qrenderaspect.h>
#include <Qt3DRender/qeffect.h>

#include <Qt3DRender/private/qsceneiohandler_p.h>
#include <Qt3DRender/private/renderviewjob_p.h>
#include <Qt3DRender/private/renderstates_p.h>
#include <Qt3DRender/private/cameraselectornode_p.h>
#include <Qt3DRender/private/framegraphvisitor_p.h>
#include <Qt3DRender/private/graphicscontext_p.h>
#include <Qt3DRender/private/cameralens_p.h>
#include <Qt3DRender/private/rendercommand_p.h>
#include <Qt3DRender/private/entity_p.h>
#include <Qt3DRender/private/renderlogging_p.h>
#include <Qt3DRender/private/material_p.h>
#include <Qt3DRender/private/renderpassfilternode_p.h>
#include <Qt3DRender/private/renderqueue_p.h>
#include <Qt3DRender/private/shader_p.h>
#include <Qt3DRender/private/buffer_p.h>
#include <Qt3DRender/private/renderstateset_p.h>
#include <Qt3DRender/private/technique_p.h>
#include <Qt3DRender/private/renderthread_p.h>
#include <Qt3DRender/private/renderview_p.h>
#include <Qt3DRender/private/techniquefilternode_p.h>
#include <Qt3DRender/private/viewportnode_p.h>
#include <Qt3DRender/private/vsyncframeadvanceservice_p.h>
#include <Qt3DRender/private/pickeventfilter_p.h>
#include <Qt3DRender/private/managers_p.h>
#include <Qt3DRender/private/buffermanager_p.h>
#include <Qt3DRender/private/nodemanagers_p.h>
#include <Qt3DRender/private/geometryrenderermanager_p.h>
#include <Qt3DRender/private/openglvertexarrayobject_p.h>

#include <Qt3DRender/qcameralens.h>
#include <Qt3DCore/private/qeventfilterservice_p.h>
#include <Qt3DCore/private/qabstractaspectjobmanager_p.h>

#include <QStack>
#include <QSurface>
#include <QElapsedTimer>
#include <QLibraryInfo>
#include <QPluginLoader>
#include <QDir>
#include <QUrl>
#include <QOffscreenSurface>
#include <QWindow>

#include <QtGui/private/qopenglcontext_p.h>

// For Debug purposes only
#include <QThread>

QT_BEGIN_NAMESPACE

using namespace Qt3DCore;

namespace Qt3DRender {
namespace Render {

const QString SCENE_PARSERS_PATH = QStringLiteral("/sceneparsers");

/*!
    \internal

    Renderer shutdown procedure:

    Since the renderer relies on the surface and OpenGLContext to perform its cleanup,
    it is shutdown when the surface is set to Q_NULLPTR

    When the surface is set to Q_NULLPTR this will request the RenderThread to terminate
    and will prevent createRenderBinJobs from returning a set of jobs as there is nothing
    more to be rendered.

    In turn, this will call shutdown which will make the OpenGL context current one last time
    to allow cleanups requiring a call to QOpenGLContext::currentContext to execute properly.
    At the end of that function, the GraphicsContext is set to null.

    At this point though, the QAspectThread is still running its event loop and will only stop
    a short while after.
 */

Renderer::Renderer(QRenderAspect::RenderType type)
    : m_services(Q_NULLPTR)
    , m_nodesManager(Q_NULLPTR)
    , m_graphicsContext(Q_NULLPTR)
    , m_renderQueue(new RenderQueue())
    , m_renderThread(type == QRenderAspect::Threaded ? new RenderThread(this) : Q_NULLPTR)
    , m_vsyncFrameAdvanceService(new VSyncFrameAdvanceService())
    , m_waitForInitializationToBeCompleted(0)
    , m_pickEventFilter(new PickEventFilter())
    , m_exposed(0)
    , m_changeSet(0)
    , m_lastFrameCorrect(0)
    , m_glContext(Q_NULLPTR)
    , m_pickBoundingVolumeJob(Q_NULLPTR)
    , m_time(0)
    , m_settings(Q_NULLPTR)
{
    // Set renderer as running - it will wait in the context of the
    // RenderThread for RenderViews to be submitted
    m_running.fetchAndStoreOrdered(1);
    if (m_renderThread)
        m_renderThread->waitForStart();
}

Renderer::~Renderer()
{

    delete m_renderQueue;
}

qint64 Renderer::time() const
{
    return m_time;
}

void Renderer::setTime(qint64 time)
{
    m_time = time;
}

NodeManagers *Renderer::nodeManagers() const
{
    return m_nodesManager;
}

void Renderer::setOpenGLContext(QOpenGLContext *context)
{
    m_glContext = context;
}

void Renderer::buildDefaultTechnique()
{
    Q_ASSERT(m_graphicsContext);
    Q_ASSERT(m_graphicsContext->openGLContext());

    // TODO: Either use public API only or just go direct to the private backend API here
    m_defaultTechnique = new QTechnique;
    m_defaultTechnique->setObjectName(QStringLiteral("default-technique"));

    QShaderProgram* defaultShader = new QShaderProgram;
    QString vertexFileName;
    QString fragmentFileName;
    if (m_graphicsContext->openGLContext()->isOpenGLES()) {
        vertexFileName = QStringLiteral("qrc:/shaders/es2/phong.vert");
        fragmentFileName = QStringLiteral("qrc:/shaders/es2/phong.frag");
    } else {
        if (m_graphicsContext->openGLContext()->format().profile() == QSurfaceFormat::CoreProfile) {
            vertexFileName = QStringLiteral("qrc:/shaders/gl3/phong.vert");
            fragmentFileName = QStringLiteral("qrc:/shaders/gl3/phong.frag");
        } else {
            vertexFileName = QStringLiteral("qrc:/shaders/es2/phong.vert");
            fragmentFileName = QStringLiteral("qrc:/shaders/es2/phong.frag");
        }
    }
    defaultShader->setVertexShaderCode(QShaderProgram::loadSource(QUrl(vertexFileName)));
    defaultShader->setFragmentShaderCode(QShaderProgram::loadSource(QUrl(fragmentFileName)));
    defaultShader->setObjectName(QStringLiteral("DefaultShader"));

    QRenderPass* basicPass = new QRenderPass;
    basicPass->setShaderProgram(defaultShader);

    m_defaultRenderStateSet = new RenderStateSet;
    m_defaultRenderStateSet->addState(getOrCreateRenderStateImpl<DepthTest>(GL_LESS));
    m_defaultRenderStateSet->addState(getOrCreateRenderStateImpl<CullFace>(GL_BACK));
    m_defaultRenderStateSet->addState(getOrCreateRenderStateImpl<ColorMask>(true, true, true, true));
    //basicPass->setStateSet(m_defaultRenderStateSet);

    m_defaultTechnique->addRenderPass(basicPass);

    QParameter* ka = new QParameter(QStringLiteral("ka"), QVector3D(0.2f, 0.2f, 0.2f));
    m_defaultTechnique->addParameter(ka);

    QParameter* kd = new QParameter(QStringLiteral("kd"), QVector3D(1.0f, 0.5f, 0.0f));
    m_defaultTechnique->addParameter(kd);

    QParameter* ks = new QParameter(QStringLiteral("ks"), QVector3D(0.01f, 0.01f, 0.01f));
    m_defaultTechnique->addParameter(ks);

    m_defaultTechnique->addParameter(new QParameter(QStringLiteral("shininess"), 150.0f));
}

void Renderer::buildDefaultMaterial()
{
    m_defaultMaterial = new QMaterial();
    m_defaultMaterial->setObjectName(QStringLiteral("DefaultMaterial"));
    m_defaultMaterial->addParameter(new QParameter(QStringLiteral("ambient"), QVector3D(0.2f, 0.2f, 0.2f)));
    m_defaultMaterial->addParameter(new QParameter(QStringLiteral("diffuse"), QVector3D(1.0f, 0.5f, 0.0f)));
    m_defaultMaterial->addParameter(new QParameter(QStringLiteral("specular"), QVector3D(0.01f, 0.01f, 0.01f)));
    m_defaultMaterial->addParameter(new QParameter(QStringLiteral("shininess"), 150.0f));

    QEffect* defEff = new QEffect;
    defEff->addTechnique(m_defaultTechnique);
    m_defaultMaterial->setEffect(defEff);
}

void Renderer::createAllocators(QAbstractAspectJobManager *jobManager)
{
    // Issue a set of jobs to create an allocator in TLS for each worker thread
    Q_ASSERT(jobManager);
    jobManager->waitForPerThreadFunction(Renderer::createThreadLocalAllocator, this);
}

void Renderer::destroyAllocators(Qt3DCore::QAbstractAspectJobManager *jobManager)
{
    // Issue a set of jobs to destroy the allocator in TLS for each worker thread
    Q_ASSERT(jobManager);
    jobManager->waitForPerThreadFunction(Renderer::destroyThreadLocalAllocator, this);
}

QThreadStorage<QFrameAllocator *> *Renderer::tlsAllocators()
{
    return &m_tlsAllocators;
}

/*!
 * For each worker thread we create a QFrameAllocatorQueue which contains m_cachedFrameCount + 1
 * QFrameAllocators. We need an additional QFrameAllocator otherwise we may be clearing the QFrameAllocator
 * of the frame we are currently rendering.
 */
void Renderer::createThreadLocalAllocator(void *renderer)
{
    Q_ASSERT(renderer);
    Renderer *theRenderer = static_cast<Renderer *>(renderer);
    if (!theRenderer->tlsAllocators()->hasLocalData()) {
        // RenderView has a sizeof 72
        // RenderCommand has a sizeof 128
        // QMatrix4x4 has a sizeof 68
        // May need to fine tune parameters passed to QFrameAllocator for best performances
        QFrameAllocator *allocator = new QFrameAllocator(192, 16, 128);
        theRenderer->tlsAllocators()->setLocalData(allocator);

        // Add the allocator to the renderer
        // so that it can be accessed
        theRenderer->addAllocator(allocator);
    }
}


/*!
 * Returns the a FrameAllocator for the caller thread.
 */
Qt3DCore::QFrameAllocator *Renderer::currentFrameAllocator()
{
    // return the QFrameAllocator for the current thread
    // It is never cleared as each renderview when it is destroyed
    // takes care of releasing anything that may have been allocated
    // using the allocator
    return m_tlsAllocators.localData();
}

void Renderer::destroyThreadLocalAllocator(void *renderer)
{
    Q_ASSERT(renderer);
    Renderer *theRenderer = static_cast<Renderer *>(renderer);
    if (theRenderer->tlsAllocators()->hasLocalData()) {
        QFrameAllocator *allocator = theRenderer->tlsAllocators()->localData();
        allocator->clear();
        // Setting the local data to null actually deletes the allocatorQeue
        // as the tls object takes ownership of pointers
        theRenderer->tlsAllocators()->setLocalData(Q_NULLPTR);
    }
}

// Called in RenderThread context by the run method of RenderThread
// RenderThread has locked the mutex already and unlocks it when this
// method termintates
void Renderer::initialize()
{
    m_graphicsContext.reset(new GraphicsContext);
    m_graphicsContext->setRenderer(this);

    QOpenGLContext* ctx = m_glContext;

    // If we are using our own context (not provided by QtQuick),
    // we need to create it
    if (!m_glContext) {
        ctx = new QOpenGLContext;
        ctx->setShareContext(qt_gl_global_share_context());

        // TO DO: Shouldn't we use the highest context available and trust
        // QOpenGLContext to fall back on the best lowest supported ?
        const QByteArray debugLoggingMode = qgetenv("QT3DRENDER_DEBUG_LOGGING");

        if (!debugLoggingMode.isEmpty()) {
            QSurfaceFormat sf = ctx->format();
            sf.setOption(QSurfaceFormat::DebugContext);
            ctx->setFormat(sf);
        }

        // Create OpenGL context
        if (ctx->create())
            qCDebug(Backend) << "OpenGL context created with actual format" << ctx->format();
        else
            qCWarning(Backend) << Q_FUNC_INFO << "OpenGL context creation failed";
    }

    // Note: we don't have a surface at this point
    // The context will be made current later on (at render time)
    m_graphicsContext->setOpenGLContext(ctx);

    // Awake setScenegraphRoot in case it was waiting
    m_waitForInitializationToBeCompleted.release(1);
    // Allow the aspect manager to proceed
    m_vsyncFrameAdvanceService->proceedToNextFrame();
}

/*!
    \internal

    Called in the context of the RenderThread to do any shutdown and cleanup
    that needs to be performed in the thread where the OpenGL context lives
*/
void Renderer::shutdown()
{
    // Clean up the graphics context
    m_graphicsContext.reset(Q_NULLPTR);
    qCDebug(Backend) << Q_FUNC_INFO << "Renderer properly shutdown";
}

void Renderer::setSurfaceExposed(bool exposed)
{
    qCDebug(Backend) << "Window exposed: " << exposed;
    m_exposed.fetchAndStoreOrdered(exposed);
}

Render::FrameGraphNode *Renderer::frameGraphRoot() const
{
    Q_ASSERT(m_settings);
    return m_nodesManager->frameGraphManager()->lookupNode(m_settings->activeFrameGraphID());
}

// QAspectThread context
// Order of execution :
// 1) RenderThread is created -> release 1 of m_waitForInitializationToBeCompleted when started
// 2) setSceneRoot waits to acquire initialization
// 3) submitRenderView -> check for surface
//    -> make surface current + create proper glHelper if needed
void Renderer::setSceneRoot(QBackendNodeFactory *factory, Entity *sgRoot)
{
    Q_ASSERT(sgRoot);

    // If initialization hasn't been completed we must wait
    m_waitForInitializationToBeCompleted.acquire();

    m_renderSceneRoot = sgRoot;
    if (!m_renderSceneRoot)
        qCWarning(Backend) << "Failed to build render scene";
    m_renderSceneRoot->dump();
    qCDebug(Backend) << Q_FUNC_INFO << "DUMPING SCENE";


    // Create the default materials ....
    // Needs a QOpenGLContext (for things like isOpenGLES ...)
    // TO DO: Maybe this should be moved elsewhere
    buildDefaultTechnique();
    buildDefaultMaterial();

    factory->createBackendNode(m_defaultMaterial);
    factory->createBackendNode(m_defaultMaterial->effect());
    factory->createBackendNode(m_defaultTechnique);
    factory->createBackendNode(m_defaultTechnique->renderPasses().constFirst());
    factory->createBackendNode(m_defaultTechnique->renderPasses().constFirst()->shaderProgram());

    // We create backend resources for all the parameters
    Q_FOREACH (QParameter *p, m_defaultMaterial->parameters())
        factory->createBackendNode(p);
    Q_FOREACH (QParameter *p, m_defaultTechnique->parameters())
        factory->createBackendNode(p);
    Q_FOREACH (QParameter *p, m_defaultMaterial->effect()->parameters())
        factory->createBackendNode(p);

    m_defaultMaterialHandle = nodeManagers()->lookupHandle<Material, MaterialManager, HMaterial>(m_defaultMaterial->id());
    m_defaultEffectHandle = nodeManagers()->lookupHandle<Effect, EffectManager, HEffect>(m_defaultMaterial->effect()->id());
    m_defaultTechniqueHandle = nodeManagers()->lookupHandle<Technique, TechniqueManager, HTechnique>(m_defaultTechnique->id());
    m_defaultRenderPassHandle = nodeManagers()->lookupHandle<RenderPass, RenderPassManager, HRenderPass>(m_defaultTechnique->renderPasses().constFirst()->id());
    m_defaultRenderShader = nodeManagers()->lookupResource<Shader, ShaderManager>(m_defaultTechnique->renderPasses().constFirst()->shaderProgram()->id());
}

void Renderer::registerEventFilter(QEventFilterService *service)
{
    qCDebug(Backend) << Q_FUNC_INFO << QThread::currentThread();
    service->registerEventFilter(m_pickEventFilter.data(), 1024);
}

void Renderer::setSettings(RenderSettings *settings)
{
    m_settings = settings;
}

RenderSettings *Renderer::settings() const
{
    return m_settings;
}

void Renderer::render()
{
    // Traversing the framegraph tree from root to lead node
    // Allows us to define the rendering set up
    // Camera, RenderTarget ...

    // Utimately the renderer should be a framework
    // For the processing of the list of renderviews

    // Matrice update, bounding volumes computation ...
    // Should be jobs

    // namespace Qt3DCore has 2 distincts node trees
    // One scene description
    // One framegraph description

    while (m_running.load() > 0) {
        doRender();
        // TO DO: Restore windows exposed detection
        // Probably needs to happens some place else though
    }
}

void Renderer::doRender()
{
    bool submissionSucceeded = false;
    uint lastBoundFBOId = 0;

    if (isReadyToSubmit()) {
        // Lock the mutex to protect access to m_surface and check if we are still set
        // to the running state and that we have a valid surface on which to draw
        // TO DO: Is that still needed given the surface changes
        QMutexLocker locker(&m_mutex);
        const QVector<Render::RenderView *> renderViews = m_renderQueue->nextFrameQueue();

        if (canRender() && (submissionSucceeded = renderViews.size() > 0) == true) {
            // Clear all dirty flags but Compute so that
            // we still render every frame when a compute shader is used in a scene
            BackendNodeDirtySet changesToUnset = m_changeSet;
            if (changesToUnset.testFlag(Renderer::ComputeDirty))
                changesToUnset.setFlag(Renderer::ComputeDirty, false);
            clearDirtyBits(changesToUnset);

            // Render using current device state and renderer configuration
            lastBoundFBOId = submitRenderViews(renderViews);
        }

        // Delete all the RenderViews which will clear the allocators
        // that were used for their allocation
        qDeleteAll(renderViews);
    }

    // Note: submissionSucceeded is false when
    // * we cannot render because a shutdown has been scheduled
    // * the renderqueue is incomplete (only when rendering with a Scene3D)
    // Otherwise returns true even for cases like
    // * No render view
    // * No surface set
    // * OpenGLContext failed to be set current
    // This behavior is important as we need to
    // call proceedToNextFrame despite rendering errors that aren't fatal

    // Only reset renderQueue and proceed to next frame if the submission
    // succeeded or it we are using a render thread.

    // If submissionSucceeded isn't true this implies that something went wrong
    // with the rendering and/or the renderqueue is incomplete from some reason
    // (in the case of scene3d the render jobs may be taking too long ....)
    if (m_renderThread || submissionSucceeded) {
        // Reset the m_renderQueue so that we won't try to render
        // with a queue used by a previous frame with corrupted content
        // if the current queue was correctly submitted
        m_renderQueue->reset();

        if (m_running.load()) { // Are we still running ?
            // Make sure that all the RenderViews, RenderCommands,
            // UniformValues ... have been completely destroyed and are leak
            // free
            // Note: we can check for non render thread cases (scene3d)
            // only when we are sure that a full frame was previously submitted
            // (submissionSucceeded == true)
            Q_FOREACH (QFrameAllocator *allocator, m_allocators)
                Q_ASSERT(allocator->isEmpty());
        }
        // We allow the RenderTickClock service to proceed to the next frame
        // In turn this will allow the aspect manager to request a new set of jobs
        // to be performed for each aspect
        m_vsyncFrameAdvanceService->proceedToNextFrame();

        // Perform the last swapBuffers calls after the proceedToNextFrame
        // as this allows us to gain a bit of time for the preparation of the
        // next frame
        // Finish up with last surface used in the list of RenderViews
        if (submissionSucceeded)
            m_graphicsContext->endDrawing(lastBoundFBOId == m_graphicsContext->defaultFBO());
    }
}

// Called by RenderViewJobs
void Renderer::enqueueRenderView(Render::RenderView *renderView, int submitOrder)
{
    QMutexLocker locker(&m_mutex); // Prevent out of order execution
    // We cannot use a lock free primitive here because:
    // - QVector is not thread safe
    // - Even if the insert is made correctly, the isFrameComplete call
    //   could be invalid since depending on the order of execution
    //   the counter could be complete but the renderview not yet added to the
    //   buffer depending on whichever order the cpu decides to process this
    if (m_renderQueue->queueRenderView(renderView, submitOrder)) {
        if (m_renderThread && m_running.load())
            Q_ASSERT(m_submitRenderViewsSemaphore.available() == 0);
        m_submitRenderViewsSemaphore.release(1);
    }
}

bool Renderer::canRender() const
{
    // Make sure that we've not been told to terminate whilst waiting on
    // the above wait condition
    if (m_renderThread && !m_running.load()) {
        qCDebug(Rendering) << "RenderThread termination requested whilst waiting";
        return false;
    }

    // TO DO: Check if all surfaces have been destroyed...
    // It may be better if the last window to be closed trigger a call to shutdown
    // Rather than having checks for the surface everywhere

    return true;
}

bool Renderer::isReadyToSubmit()
{
    // If we are using a render thread, make sure that
    // we've been told to render before rendering
    if (m_renderThread) { // Prevent ouf of order execution
        m_submitRenderViewsSemaphore.acquire(1);
        // When using Thread rendering, the semaphore should only
        // be released when the frame queue is complete and there's
        // something to render
        // The case of shutdown should have been handled just before
        Q_ASSERT(m_renderQueue->isFrameQueueComplete());
    } else {
        // When using synchronous rendering (QtQuick)
        // We are not sure that the frame queue is actually complete
        // Since a call to render may not be synched with the completions
        // of the RenderViewJobs
        // In such a case we return early, waiting for a next call with
        // the frame queue complete at this point
        QMutexLocker locker(&m_mutex);
        if (!m_renderQueue->isFrameQueueComplete())
            return false;
    }
    return true;
}

// Happens in RenderThread context when all RenderViewJobs are done
// Returns the id of the last bound FBO
uint Renderer::submitRenderViews(const QVector<Render::RenderView *> &renderViews)
{
    QElapsedTimer timer;
    quint64 queueElapsed = 0;
    timer.start();

    const int renderViewsCount = renderViews.size();
    quint64 frameElapsed = queueElapsed;
    m_lastFrameCorrect.store(1);    // everything fine until now.....

    qCDebug(Memory) << Q_FUNC_INFO << "rendering frame ";

    // We might not want to render on the default FBO
    bool boundFboIdValid = false;
    uint lastBoundFBOId = 0;
    QColor previousClearColor = renderViews.first()->clearColor();
    QSurface *surface = Q_NULLPTR;
    QSurface *previousSurface = Q_NULLPTR;
    for (int i = 0; i < renderViewsCount; ++i) {
        // Initialize GraphicsContext for drawing
        // If the RenderView has a RenderStateSet defined
        const RenderView *renderView = renderViews.at(i);

        // Check if using the same surface as the previous RenderView.
        // If not, we have to free up the context from the previous surface
        // and make the context current on the new surface
        surface = renderView->surface();

        // TO DO: Make sure that the surface we are rendering too has not been unset

        // For now, if we do not have a surface, skip this renderview
        // TODO: Investigate if it's worth providing a fallback offscreen surface
        //       to use when surface is null. Or if we should instead expose an
        //       offscreensurface to Qt3D.
        if (!surface) {
            m_lastFrameCorrect.store(0);
            continue;
        }

        if (surface != previousSurface && previousSurface)
            m_graphicsContext->endDrawing(lastBoundFBOId == m_graphicsContext->defaultFBO());

        if (surface != previousSurface) {
            // If we can't make the context current on the surface, skip to the
            // next RenderView. We won't get the full frame but we may get something
            if (!m_graphicsContext->beginDrawing(surface, previousClearColor)) {
                qWarning() << "Failed to make OpenGL context current on surface";
                m_lastFrameCorrect.store(0);
                continue;
            }

            previousSurface = surface;

            if (!boundFboIdValid) {
                boundFboIdValid = true;
                lastBoundFBOId = m_graphicsContext->boundFrameBufferObject();
            }

            // Reset state to the default state
            m_graphicsContext->setCurrentStateSet(m_defaultRenderStateSet);
        }

        // Set RenderView render state
        RenderStateSet *renderViewStateSet = renderView->stateSet();
        if (renderViewStateSet)
            m_graphicsContext->setCurrentStateSet(renderViewStateSet);

        // Set RenderTarget ...
        // Activate RenderTarget
        m_graphicsContext->activateRenderTarget(nodeManagers()->data<RenderTarget, RenderTargetManager>(renderView->renderTargetHandle()),
                                                renderView->attachmentPack(),
                                                lastBoundFBOId);

        // Set clear color if different
        if (previousClearColor != renderView->clearColor()) {
            previousClearColor = renderView->clearColor();
            m_graphicsContext->clearColor(previousClearColor);
        }

        // Clear BackBuffer
        m_graphicsContext->clearBackBuffer(renderView->clearBuffer());

        // Set the Viewport
        m_graphicsContext->setViewport(renderView->viewport(), renderView->surfaceSize() * renderView->devicePixelRatio());

        // Execute the render commands
        if (!executeCommands(renderView))
            m_lastFrameCorrect.store(0);    // something went wrong; make sure to render the next frame!

        // executeCommands takes care of restoring the stateset to the value
        // of gc->currentContext() at the moment it was called (either
        // renderViewStateSet or m_defaultRenderStateSet)

        frameElapsed = timer.elapsed() - frameElapsed;
        qCDebug(Rendering) << Q_FUNC_INFO << "Submitted Renderview " << i + 1 << "/" << renderViewsCount  << "in " << frameElapsed << "ms";
        frameElapsed = timer.elapsed();
    }

    if (surface) {
        // Reset state to the default state if the last stateset is not the
        // defaultRenderStateSet
        if (m_graphicsContext->currentStateSet() != m_defaultRenderStateSet)
            m_graphicsContext->setCurrentStateSet(m_defaultRenderStateSet);
    }

    queueElapsed = timer.elapsed() - queueElapsed;
    qCDebug(Rendering) << Q_FUNC_INFO << "Submission of Queue in " << queueElapsed << "ms <=> " << queueElapsed / renderViewsCount << "ms per RenderView <=> Avg " << 1000.0f / (queueElapsed * 1.0f/ renderViewsCount * 1.0f) << " RenderView/s";
    qCDebug(Rendering) << Q_FUNC_INFO << "Submission Completed in " << timer.elapsed() << "ms";

    return lastBoundFBOId;
}

void Renderer::markDirty(BackendNodeDirtySet changes, BackendNode *node)
{
    Q_UNUSED(node);
    m_changeSet |= changes;
}

Renderer::BackendNodeDirtySet Renderer::dirtyBits()
{
    return m_changeSet;
}

void Renderer::clearDirtyBits(BackendNodeDirtySet changes)
{
    m_changeSet &= ~changes;
}

bool Renderer::shouldRender()
{
    // Only render if something changed during the last frame, or the last frame
    // was not rendered successfully
    // TODO: Reinstate the proper logic here when it catches all cases
    return true;
    // was not rendered successfully (or render-on-demand is disabled)
    return (m_settings->renderPolicy() == QRenderSettings::Always
            || m_changeSet != 0
            || !m_lastFrameCorrect.load());
}

void Renderer::skipNextFrame()
{
    Q_ASSERT(m_settings->renderPolicy() != QRenderSettings::Always);

    // make submitRenderViews() actually run
    m_renderQueue->setNoRender();
    m_submitRenderViewsSemaphore.release(1);
}

// Waits to be told to create jobs for the next frame
// Called by QRenderAspect jobsToExecute context of QAspectThread
QVector<Qt3DCore::QAspectJobPtr> Renderer::renderBinJobs()
{
    QVector<QAspectJobPtr> renderBinJobs;

    // Traverse the current framegraph. For each leaf node create a
    // RenderView and set its configuration then create a job to
    // populate the RenderView with a set of RenderCommands that get
    // their details from the RenderNodes that are visible to the
    // Camera selected by the framegraph configuration

    FrameGraphVisitor visitor;
    visitor.traverse(frameGraphRoot(), this, &renderBinJobs);

    // Set target number of RenderViews
    m_renderQueue->setTargetRenderViewCount(renderBinJobs.size());
    return renderBinJobs;
}

QAspectJobPtr Renderer::pickBoundingVolumeJob()
{
    // Clear any previous dependency not valid anymore
    if (!m_pickBoundingVolumeJob)
        m_pickBoundingVolumeJob.reset(new PickBoundingVolumeJob(this));
    m_pickBoundingVolumeJob->removeDependency(QWeakPointer<QAspectJob>());
    m_pickBoundingVolumeJob->setRoot(m_renderSceneRoot);
    return m_pickBoundingVolumeJob;
}

// Called during while traversing the FrameGraph for each leaf node context of QAspectThread
Qt3DCore::QAspectJobPtr Renderer::createRenderViewJob(FrameGraphNode *node, int submitOrderIndex)
{
    RenderViewJobPtr job(new RenderViewJob);
    job->setRenderer(this);
    //    if (m_surface)
    //        job->setSurfaceSize(m_surface->size());
    // TO DO: the surface size can only be set by the RenderView
    // since the only the RenderView will know about the surface
    // it should be renderer onto
    job->setFrameGraphLeafNode(node);
    job->setSubmitOrderIndex(submitOrderIndex);
    return job;
}

QAbstractFrameAdvanceService *Renderer::frameAdvanceService() const
{
    return static_cast<Qt3DCore::QAbstractFrameAdvanceService *>(m_vsyncFrameAdvanceService.data());
}

// Called by executeCommands
void Renderer::performDraw(GeometryRenderer *rGeometryRenderer, GLsizei primitiveCount, Attribute *indexAttribute)
{
    const GLint primType = rGeometryRenderer->primitiveType();
    const bool drawIndexed = indexAttribute != Q_NULLPTR;
    const GLint indexType = drawIndexed ? GraphicsContext::glDataTypeFromAttributeDataType(indexAttribute->vertexBaseType()) : 0;

    if (rGeometryRenderer->primitiveType() == QGeometryRenderer::Patches)
        m_graphicsContext->setVerticesPerPatch(rGeometryRenderer->verticesPerPatch());

    if (rGeometryRenderer->primitiveRestart())
        m_graphicsContext->enablePrimitiveRestart(rGeometryRenderer->restartIndex());

    // TO DO: Add glMulti Draw variants
    if (drawIndexed)
        m_graphicsContext->drawElementsInstanced(primType,
                                                 primitiveCount,
                                                 indexType,
                                                 reinterpret_cast<void*>(quintptr(indexAttribute->byteOffset())),
                                                 rGeometryRenderer->instanceCount(),
                                                 rGeometryRenderer->baseVertex());
    else
        m_graphicsContext->drawArraysInstanced(primType,
                                               rGeometryRenderer->baseInstance(),
                                               primitiveCount,
                                               rGeometryRenderer->instanceCount());

#if defined(QT3D_RENDER_ASPECT_OPENGL_DEBUG)
    int err = m_graphicsContext->openGLContext()->functions()->glGetError();
    if (err)
        qCWarning(Rendering) << "GL error after drawing mesh:" << QString::number(err, 16);
#endif

    if (rGeometryRenderer->primitiveRestart())
        m_graphicsContext->disablePrimitiveRestart();

    // Unset dirtiness on rGeometryRenderer only
    // The rGeometry may be shared by several rGeometryRenderer
    // so we cannot unset its dirtiness at this point
    rGeometryRenderer->unsetDirty();
}

void Renderer::performCompute(const RenderView *rv, RenderCommand *command)
{
    Shader *shader = m_nodesManager->data<Shader, ShaderManager>(command->m_shader);
    if (shader != Q_NULLPTR) {
        m_graphicsContext->activateShader(shader);
        m_graphicsContext->setParameters(command->m_parameterPack);
        m_graphicsContext->dispatchCompute(command->m_workGroups[0],
                command->m_workGroups[1],
                command->m_workGroups[2]);

        // HACK: Reset the compute flag to dirty
        m_changeSet |= AbstractRenderer::ComputeDirty;

#if defined(QT3D_RENDER_ASPECT_OPENGL_DEBUG)
        int err = m_graphicsContext->openGLContext()->functions()->glGetError();
        if (err)
            qCWarning(Rendering) << "GL error after drawing mesh:" << QString::number(err, 16);
#endif
    }
}

bool Renderer::createOrUpdateVAO(RenderCommand *command,
                                 HVao *previousVaoHandle,
                                 OpenGLVertexArrayObject **vao)
{
    VAOManager *vaoManager = m_nodesManager->vaoManager();
    if (m_graphicsContext->supportsVAO()) {
        command->m_vao = vaoManager->lookupHandle(QPair<HGeometry, HShader>(command->m_geometry, command->m_shader));

        if (command->m_vao.isNull()) {
            qCDebug(Rendering) << Q_FUNC_INFO << "Allocating new VAO";
            command->m_vao = vaoManager->getOrAcquireHandle(QPair<HGeometry, HShader>(command->m_geometry, command->m_shader));
            vaoManager->data(command->m_vao)->setVao(new QOpenGLVertexArrayObject());
            vaoManager->data(command->m_vao)->create();
        }

        if (*previousVaoHandle != command->m_vao) {
            *previousVaoHandle = command->m_vao;
            *vao = vaoManager->data(command->m_vao);
            return true;
        }
        Q_ASSERT(*vao);
    }
    return false;
}

// Called by RenderView->submit() in RenderThread context
// Returns true, if all RenderCommands were sent to the GPU
bool Renderer::executeCommands(const RenderView *rv)
{
    bool allCommandsIssued = true;

    // Render drawing commands
    const QVector<RenderCommand *> commands = rv->commands();

    // Use the graphicscontext to submit the commands to the underlying
    // graphics API (OpenGL)

    // Save the RenderView base stateset
    RenderStateSet *globalState = m_graphicsContext->currentStateSet();
    OpenGLVertexArrayObject *vao = Q_NULLPTR;
    HVao previousVaoHandle;

    Q_FOREACH (RenderCommand *command, commands) {

        if (command->m_type == RenderCommand::Compute) { // Compute Call
            performCompute(rv, command);
        } else { // Draw Command

            // Check if we have a valid GeometryRenderer + Geometry
            Geometry *rGeometry = m_nodesManager->data<Geometry, GeometryManager>(command->m_geometry);
            GeometryRenderer *rGeometryRenderer = m_nodesManager->data<GeometryRenderer, GeometryRendererManager>(command->m_geometryRenderer);
            const bool hasGeometryRenderer = rGeometry != Q_NULLPTR && rGeometryRenderer != Q_NULLPTR && !rGeometry->attributes().isEmpty();

            if (!hasGeometryRenderer) {
                allCommandsIssued = false;
                qCWarning(Rendering) << "RenderCommand should have a mesh to render";
                continue;
            }

            Shader *shader = m_nodesManager->data<Shader, ShaderManager>(command->m_shader);
            if (shader == Q_NULLPTR) {
                shader = m_defaultRenderShader;
                command->m_parameterPack = m_defaultUniformPack;
            }

            // The VAO should be created only once for a QGeometry and a ShaderProgram
            // Manager should have a VAO Manager that are indexed by QMeshData and Shader
            // RenderCommand should have a handle to the corresponding VAO for the Mesh and Shader
            const bool needsToBindVAO = createOrUpdateVAO(command, &previousVaoHandle, &vao);

            //// We activate the shader here
            // This will fill the attributes & uniforms info the first time the shader is loaded
            m_graphicsContext->activateShader(shader);

            //// Initialize GL
            // The initialization is performed only once parameters in the command are set
            // Which indicates that the shader has been initialized and that renderview jobs were able to retrieve
            // Uniform and Attributes info from the shader
            // Otherwise we might create a VAO without attribute bindings as the RenderCommand had no way to know about attributes
            // Before the shader was loader
            Attribute *indexAttribute = Q_NULLPTR;
            bool specified = false;
            const bool requiresVAOUpdate = (!vao || !vao->isSpecified()) || (rGeometry->isDirty() || rGeometryRenderer->isDirty());
            GLsizei primitiveCount = rGeometryRenderer->primitiveCount();

            // Append dirty Geometry to temporary vector
            // so that its dirtiness can be unset later
            if (rGeometry->isDirty())
                m_dirtyGeometry.push_back(rGeometry);

            if (needsToBindVAO && vao != Q_NULLPTR)
                vao->bind();

            if (!command->m_attributes.isEmpty()) {
                // Update or set Attributes and Buffers for the given rGeometry and Command
                indexAttribute = updateBuffersAndAttributes(rGeometry, command, primitiveCount, requiresVAOUpdate);
                specified = true;
                if (vao)
                    vao->setSpecified(true);
            }

            //// Update program uniforms
            m_graphicsContext->setParameters(command->m_parameterPack);

            //// OpenGL State
            // Set state
            RenderStateSet *localState = command->m_stateSet;
            // Merge the RenderCommand state with the globalState of the RenderView
            // Or restore the globalState if no stateSet for the RenderCommand
            if (localState != Q_NULLPTR) {
                command->m_stateSet->merge(globalState);
                m_graphicsContext->setCurrentStateSet(command->m_stateSet);
            } else {
                m_graphicsContext->setCurrentStateSet(globalState);
            }
            // All Uniforms for a pass are stored in the QUniformPack of the command
            // Uniforms for Effect, Material and Technique should already have been correctly resolved
            // at that point

            //// Draw Calls
            if (primitiveCount && (specified || (vao && vao->isSpecified()))) {
                performDraw(rGeometryRenderer, primitiveCount, indexAttribute);
            } else {
                allCommandsIssued = false;
            }
        }
    } // end of RenderCommands loop

    // We cache the VAO and release it only at the end of the exectute frame
    // We try to minimize VAO binding between RenderCommands
    if (vao)
        vao->release();

    // Reset to the state we were in before executing the render commands
    m_graphicsContext->setCurrentStateSet(globalState);

    // Unset dirtiness on Geometry and Attributes
    Q_FOREACH (Attribute *attribute, m_dirtyAttributes)
        attribute->unsetDirty();
    m_dirtyAttributes.clear();

    Q_FOREACH (Geometry *geometry, m_dirtyGeometry)
        geometry->unsetDirty();
    m_dirtyGeometry.clear();

    return allCommandsIssued;
}

Attribute *Renderer::updateBuffersAndAttributes(Geometry *geometry, RenderCommand *command, GLsizei &count, bool forceUpdate)
{
    Attribute *indexAttribute = Q_NULLPTR;
    uint estimatedCount = 0;

    m_dirtyAttributes.reserve(m_dirtyAttributes.size() + geometry->attributes().size());
    Q_FOREACH (QNodeId attributeId, geometry->attributes()) {
        // TO DO: Improvement we could store handles and use the non locking policy on the attributeManager
        Attribute *attribute = m_nodesManager->attributeManager()->lookupResource(attributeId);

        if (attribute == Q_NULLPTR)
            continue;

        Buffer *buffer = m_nodesManager->bufferManager()->lookupResource(attribute->bufferId());

        if (buffer == Q_NULLPTR)
            continue;

        if (buffer->isDirty()) {
            // Reupload buffer data
            m_graphicsContext->updateBuffer(buffer);

            // Clear dirtiness of buffer so that it is not reuploaded every frame
            buffer->unsetDirty();
        }

        // Update attribute and create buffer if needed

        // Index Attribute
        if (attribute->attributeType() == QAttribute::IndexAttribute) {
            if (attribute->isDirty() || forceUpdate)
                m_graphicsContext->specifyIndices(buffer);
            indexAttribute = attribute;
            // Vertex Attribute
        } else if (command->m_attributes.contains(attribute->nameId())) {
            if (attribute->isDirty() || forceUpdate)
                m_graphicsContext->specifyAttribute(attribute, buffer, attribute->name());
            estimatedCount = qMax(attribute->count(), estimatedCount);
        }

        // Append attribute to temporary vector so that its dirtiness
        // can be cleared at the end of the frame
        m_dirtyAttributes.push_back(attribute);

        // Note: We cannont call unsertDirty on the Attributeat this
        // point as we don't know if the attributes are being shared
        // with other geometry / geometryRenderer in which case they still
        // should remain dirty so that VAO for these commands are properly
        // updated
    }

    // If the count was not specified by the geometry renderer
    // we set it to what we estimated it to be
    if (count == 0)
        count = indexAttribute ? indexAttribute->count() : estimatedCount;
    return indexAttribute;
}

void Renderer::addAllocator(Qt3DCore::QFrameAllocator *allocator)
{
    QMutexLocker lock(&m_mutex);
    m_allocators.append(allocator);
}

QList<QMouseEvent> Renderer::pendingPickingEvents() const
{
    return m_pickEventFilter->pendingEvents();
}

QGraphicsApiFilter *Renderer::contextInfo() const
{
    return m_graphicsContext->contextInfo();
}

} // namespace Render
} // namespace Qt3DRender

QT_END_NAMESPACE
