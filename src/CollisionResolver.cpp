#include "CollisionResolver.h"
#include <cmath>

using namespace irr;
using namespace core;
using namespace scene;
using namespace video;
using namespace io;
using namespace gui;


#define debug
#ifdef debug
std::ostream& operator<<(std::ostream& os, const irr::core::vector3df& v)
{
    os << "(" << v.X << " " << v.Y << " " << v.Z << ")";
    return os;
}

std::ostream& operator<<(std::ostream& os, const btVector3& v)
{
    os << "(" << v.getX() << " " << v.getY() << " " << v.getZ() << ")";
    return os;
}
#endif

gg::MCollisionResolver::MCollisionResolver(IrrlichtDevice* irrDev, btDiscreteDynamicsWorld* btDDW,
                                           MObjectCreator* creator, std::vector<std::unique_ptr<MObject>>* objs)
    : m_irrDevice(irrDev),
      m_btWorld(btDDW),
      m_objectCreator(creator),
      m_objects(objs)
{
    m_done.store(false);
    m_subtractor1 = std::move(std::thread([this] { meshSubtractor(); }));
    m_subtractor2 = std::move(std::thread([this] { meshSubtractor(); }));
}

gg::MCollisionResolver::~MCollisionResolver()
{
    m_done.store(true);
    m_subtractor1.join();
    m_subtractor2.join();
}

void gg::MCollisionResolver::resolveCollision(MObject* obj, btVector3 point, btVector3 from,
                                              btScalar impulse, MObject* other)
{
    std::lock_guard<std::mutex> objlock(obj->m_mutex);
    std::lock_guard<std::mutex> otherlock(other->m_mutex);
    MObject::Material material = obj->getMaterial();
    if(other->isDeleted())
    {
        return;
    }
    else if(material == MObject::Material::GROUND)
    {
        return;
    }
    else if(material != MObject::Material::SHIP && impulse > 0 )
    {
        if(other->getMaterial() == MObject::Material::SHOT)
        {
            m_btWorld->removeCollisionObject(other->getRigid());
            other->removeNode();
            other->setDeleted();
        }
        if(obj->isMesh() && (other->getMaterial() != MObject::Material::GROUND || impulse > 200 || other->getMaterial() == MObject::Material::SHOT))
        {
            using namespace voro;
                float cube_size = 2.5f;
                vector3df cube_min(point.getX()-cube_size, point.getY()-cube_size, point.getZ()-cube_size);
                vector3df cube_max(point.getX()+cube_size, point.getY()+cube_size, point.getZ()+cube_size);

                container con(cube_min.X, cube_max.X,
                              cube_min.Y, cube_max.Y,
                              cube_min.Z, cube_max.Z,
                              8,8,8,
                              false,false,false,
                              8);
                con.put(0,point.getX(),point.getY(),point.getZ());
                //for(auto&& i : {1, 2})
                {
                 //   con.put(i,std::fmod(rand(),cube_size) + cube_min.X, std::fmod(rand(),cube_size) + cube_min.Y, std::fmod(rand(),cube_size) + cube_min.Z);
                }
                c_loop_all loop(con);
                loop.start();
                voronoicell c;
                con.compute_cell(c,loop);
                IMesh* mesh = gg::MeshManipulators::convertMesh(c);

            //generateDebree(mesh,point,(from-point)*3, obj->getMaterial());
            std::lock_guard<std::mutex> lock (m_taskQueueMutex);
            vector3df position(vector3df(loop.x(),loop.y(),loop.z()) - obj->getNode()->getPosition());
            m_subtractionTasks.push_back(std::make_tuple(obj, mesh, position));
        }
    }
}

void gg::MCollisionResolver::generateDebree(IMesh* mesh, btVector3 point, btVector3 impulse, MObject::Material material)
{

    IMesh* fragment_mesh = m_irrDevice->getSceneManager()->getMeshManipulator()->createMeshUniquePrimitives(mesh);
    m_irrDevice->getSceneManager()->getMeshManipulator()->scale(fragment_mesh,vector3df(0.5,0.5,0.5));
    MObject* fragment = m_objectCreator->createMeshRigidBody(fragment_mesh, point, 30, material);
    m_objects->push_back(std::unique_ptr<MObject>(fragment));
    m_btWorld->addRigidBody(fragment->getRigid());
    fragment->getRigid()->applyImpulse(impulse, point);
}

void gg::MCollisionResolver::meshSubtractor()
{
    MObject* obj;
    IMesh* mesh;
    vector3df position;
    MeshManipulators::Nef_polyhedron* oldPoly;
    std::unique_lock<std::mutex> taskLock(m_taskQueueMutex, std::defer_lock);
    while(!m_done)
    {
        taskLock.lock();
        if(m_subtractionTasks.size() > 0)
        {
            std::tie(obj, mesh, position) = m_subtractionTasks.front();
            m_subtractionTasks.pop_front();
            taskLock.unlock();
            std::lock_guard<std::mutex> objlock(obj->m_mutex);
            if(obj->isDeleted())
            {
              return;
            }
            oldPoly = &obj->getPolyhedron();
            int old_version = obj->version.load();
            try
            {
                MeshManipulators::Nef_polyhedron newPoly = MeshManipulators::subtractMesh(*oldPoly, mesh, position);
                std::vector<MeshManipulators::Nef_polyhedron> newNefPolyhedrons(std::move(MeshManipulators::splitPolyhedron(std::move(newPoly))));
                for(size_t i = 0; i < newNefPolyhedrons.size(); i++)
                {
                    IMesh* new_mesh;
                    vector3df center;
                    std::tie(new_mesh, center) = MeshManipulators::convertPolyToMesh(newNefPolyhedrons[i]);
                    btVector3 btCenter(center.X, center.Y, center.Z);
                    std::lock_guard<std::mutex> resLock(m_resultQueueMutex);
                    if(i == 0)
                    {
                        m_subtractionResults.push_back(std::make_tuple(obj, obj->getRigid()->getCenterOfMassPosition(),
                                                                       std::move(newNefPolyhedrons[0]), new_mesh, old_version));
                    }
                    else
                    {
                        m_subtractionResults.push_back(
                                    std::make_tuple(new MObject(NULL, NULL, obj->getMaterial(), false),
                                                    obj->getRigid()->getCenterOfMassPosition() + btCenter,
                                                    std::move(newNefPolyhedrons[i]), new_mesh, old_version));
                    }
                }
            }
           catch(...)
           {
               std::cout << "FAILED\n";
           }
        }
        else
        {
            taskLock.unlock();
            std::this_thread::yield();
        }
    }
}

void gg::MCollisionResolver::subtractionApplier()
{

    MObject* obj = NULL;
    IMesh* new_mesh = NULL;
    btVector3 position;
    int old_version;
    MeshManipulators::Nef_polyhedron newPoly;
    {
        std::lock_guard<std::mutex> resLock(m_resultQueueMutex);
        if(m_subtractionResults.size() > 0)
        {
            std::tie(obj,position, newPoly, new_mesh, old_version) = m_subtractionResults.front();
            m_subtractionResults.pop_front();
        }
    }
    if(obj && new_mesh)
    {
        if(obj->version > old_version)
        {
            return;
        }
        std::lock_guard<std::mutex> objLock(obj->m_mutex);
        obj->setPolyhedron(std::move(newPoly));

        if(obj->getRigid())
        {
            btRigidBody* body = obj->getRigid();
            IMeshSceneNode* Node = static_cast<IMeshSceneNode*>(obj->getNode());
            Node->setMesh(new_mesh);
            Node->setMaterialType(EMT_SOLID);
            Node->setMaterialFlag(EMF_LIGHTING, 0);
            Node->setMaterialFlag(EMF_NORMALIZE_NORMALS, true);
            btCollisionShape *Shape = MeshManipulators::nefToShape(newPoly);
            Shape->setMargin(0.05f);
            delete body->getCollisionShape();
            body->setCollisionShape(Shape);
            obj->version++;
        }
        else
        {
            obj = m_objectCreator->createMeshRigidBody(new_mesh, position, 10, obj->getMaterial());
            obj->setPolyhedron(newPoly);
            m_objects->push_back(std::unique_ptr<MObject>(obj));
            m_btWorld->addRigidBody(obj->getRigid());
        }
    }
}

bool gg::MCollisionResolver::isInside(btRigidBody* body, btVector3 point,btVector3 from)
{
    int in = 0;
    btVector3 last = from;

    btCollisionWorld::AllHitsRayResultCallback clbck(from, point); //entering body
    m_btWorld->rayTest(from, point, clbck);
    for(int i = 0; i < clbck.m_hitPointWorld.size(); i++)
    {
        if(int(last.getX()*100) != int(clbck.m_hitPointWorld[i].getX()*100) || //this can fail on extra thin walls
           int(last.getY()*100) != int(clbck.m_hitPointWorld[i].getY()*100) || //we have to filter almost equal positions because edges and vertices count multiple times
           int(last.getZ()*100) != int(clbck.m_hitPointWorld[i].getZ()*100))
        {
            last = clbck.m_hitPointWorld[i];
            in++;
        }
    }
    return in%2;
}

void gg::MCollisionResolver::resolveAll()
{

    int numManifolds = m_btWorld->getDispatcher()->getNumManifolds();
    //For each contact manifold
    for (int i = 0; i < numManifolds; i++)
    {
        btPersistentManifold* contactManifold = m_btWorld->getDispatcher()->getManifoldByIndexInternal(i);
        if(contactManifold->getBody0() && contactManifold->getBody1())
        {
            const btRigidBody* obA = static_cast<const btRigidBody*>(contactManifold->getBody0());
            const btRigidBody* obB = static_cast<const btRigidBody*>(contactManifold->getBody1());
            contactManifold->refreshContactPoints(obA->getWorldTransform(), obB->getWorldTransform());
            btManifoldPoint& pt = contactManifold->getContactPoint(0);
            btVector3 ptA = pt.getPositionWorldOnA();
            btVector3 ptB = pt.getPositionWorldOnB();
            btScalar impulse = pt.getAppliedImpulse();

            MObject* objectA, *objectB;
            objectA = static_cast<MObject*>(obA->getUserPointer());
            objectB = static_cast<MObject*>(obB->getUserPointer());

            if(obA->getUserPointer())
            {
                objectA = static_cast<MObject*>(obA->getUserPointer());
            }
            if(obB->getUserPointer())
            {
                objectB = static_cast<MObject*>(obB->getUserPointer());
            }

            if(!objectA->isDeleted())
            {
                resolveCollision(objectA, ptA, ptA-ptB, impulse, objectB);
            }
            if(!objectB->isDeleted())
            {
                resolveCollision(objectB, ptB, ptB-ptA, impulse, objectA);
            }
        }
    }
    subtractionApplier();
}

//DUST GENERATOR
/*    scene::IParticleSystemSceneNode* ps =
    m_irrDevice->getSceneManager()->addParticleSystemSceneNode(false);

    scene::IParticleEmitter* em = ps->createSphereEmitter(
        vector3df(point.getX(),point.getY(),point.getZ()),
        2.f,
        core::vector3df(0.0f,0.0f,0.0f),   // initial direction
        1000,10000,                             // emit rate
        video::SColor(0,0,0,0),       // darkest color
        video::SColor(0,100,100,100),       // brightest color
        1000,2000,0,                         // min and max age, angle
        core::dimension2df(1,1),         // min size
        core::dimension2df(3.f,3.f));        // max size

    ps->setEmitter(em); // this grabs the emitter
    em->drop(); // so we can drop it here without deleting it

    scene::IParticleAffector* paf = ps->createFadeOutParticleAffector();

    ps->addAffector(paf); // same goes for the affector
    paf->drop();

    ps->setPosition(obj->getNode()->getAbsolutePosition());
    ps->setScale(core::vector3df(1,1,1));
    ps->setMaterialFlag(video::EMF_LIGHTING, false);
    ps->setMaterialFlag(video::EMF_ZWRITE_ENABLE, false);
    ps->setMaterialTexture(0, m_irrDevice->getVideoDriver()->getTexture("media/dust2.png"));
    ps->setMaterialType(video::EMT_TRANSPARENT_ADD_COLOR);
    ps->doParticleSystem(1000);
    */
