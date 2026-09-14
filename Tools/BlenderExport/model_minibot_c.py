"""MiniBot C の分割装甲・関節・一刀を新規作成する。

Author: Hasegawa Jin
Date: 2026-09-13
"""

import bpy
import math
from mathutils import Vector, Matrix
from mathutils.geometry import tessellate_polygon

SCENE = None
MODEL = None
RIG = None
MAT = {}
PARTS = []


def Material(name, color, metal=0, rough=.35, emission=0):
    m = bpy.data.materials.get('C_' + name) or bpy.data.materials.new('C_' + name)
    m.use_nodes = True
    m.diffuse_color = (*color, 1)
    p = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    p.inputs['Base Color'].default_value = (*color, 1)
    p.inputs['Metallic'].default_value = metal
    p.inputs['Roughness'].default_value = rough
    p.inputs['Coat Weight'].default_value = .18 if name == 'Porcelain' else .06
    p.inputs['Coat Roughness'].default_value = .23
    if emission:
        p.inputs['Emission Color'].default_value = (*color, 1)
        p.inputs['Emission Strength'].default_value = emission
    return m


def Bind(obj, bone):
    if bone:
        group = obj.vertex_groups.new(name=bone)
        group.add(list(range(len(obj.data.vertices))), 1.0, 'REPLACE')
        modifier = obj.modifiers.new('Humanoid skin', 'ARMATURE')
        modifier.object = RIG
        obj.parent = RIG
        obj['body_bone'] = bone
    PARTS.append(obj)
    return obj


def Mesh(name, vertices, faces, material, bone=None, bevel=.0, smooth=True, collection=None):
    data = bpy.data.meshes.new('C_' + name)
    data.from_pydata(vertices, [], faces)
    data.update()
    obj = bpy.data.objects.new('C_' + name, data)
    (collection or MODEL).objects.link(obj)
    data.materials.append(MAT[material])
    for p in data.polygons:
        p.use_smooth = smooth
    if bevel:
        mod = obj.modifiers.new('Machined edges', 'BEVEL')
        mod.width = bevel
        mod.segments = 3
        mod.limit_method = 'ANGLE'
        mod.angle_limit = .5
        mod.harden_normals = True
        wn = obj.modifiers.new('Surface normals', 'WEIGHTED_NORMAL')
        wn.keep_sharp = True
        wn.weight = 35
    return Bind(obj, bone)


def Loft(name, sections, material, bone=None, segments=24, square=.0, bevel=0):
    vertices = []
    for z, rx, ry, cx, cy in sections:
        for j in range(segments):
            a = math.tau*j/segments
            c, s = math.cos(a), math.sin(a)
            if square:
                c = math.copysign(abs(c)**square, c)
                s = math.copysign(abs(s)**square, s)
            vertices.append((cx+rx*c, cy+ry*s, z))
    faces = []
    for i in range(len(sections)-1):
        for j in range(segments):
            k = i*segments+j
            faces.append((k, i*segments+(j+1)%segments,
                          (i+1)*segments+(j+1)%segments, k+segments))
    faces.extend([tuple(reversed(range(segments))),
                  tuple((len(sections)-1)*segments+j for j in range(segments))])
    return Mesh(name, vertices, faces, material, bone, bevel)


def Axial(name, a, b, profile, material, bone=None, segments=16, square=1, bevel=.003):
    a, b = Vector(a), Vector(b)
    d = (b-a).normalized()
    u = Vector((1, 0, 0))
    if abs(d.dot(u)) > .97:
        u = Vector((0, 1, 0))
    u = (u-d*u.dot(d)).normalized()
    v = d.cross(u).normalized()
    vertices = []
    for t, rx, ry in profile:
        center = a.lerp(b, t)
        for j in range(segments):
            angle = math.tau*j/segments
            x, y = math.cos(angle), math.sin(angle)
            x = math.copysign(abs(x)**square, x)
            y = math.copysign(abs(y)**square, y)
            vertices.append(center + u*rx*x + v*ry*y)
    faces = []
    for i in range(len(profile)-1):
        for j in range(segments):
            k = i*segments+j
            faces.append((k, i*segments+(j+1)%segments,
                          (i+1)*segments+(j+1)%segments, k+segments))
    faces.extend([tuple(reversed(range(segments))),
                  tuple((len(profile)-1)*segments+j for j in range(segments))])
    return Mesh(name, vertices, faces, material, bone, bevel)


def Cylinder(name, center, axis, radius, depth, material, bone=None, segments=24):
    c, d = Vector(center), Vector(axis).normalized()*depth/2
    return Axial(name, c-d, c+d, [(0, radius*.94, radius*.94),
                 (.15, radius, radius), (.85, radius, radius), (1, radius*.94, radius*.94)],
                 material, bone, segments, bevel=.001)


def Ball(name, center, scale, material, bone=None, segments=24, rings=12):
    vertices, faces = [], []
    c = Vector(center)
    for i in range(rings+1):
        phi = math.pi*(i+.02)/(rings+.04)
        for j in range(segments):
            theta = math.tau*j/segments
            vertices.append(c+Vector((scale[0]*math.sin(phi)*math.cos(theta),
                                     scale[1]*math.sin(phi)*math.sin(theta), scale[2]*math.cos(phi))))
    for i in range(rings):
        for j in range(segments):
            k = i*segments+j
            faces.append((k, i*segments+(j+1)%segments,
                          (i+1)*segments+(j+1)%segments, k+segments))
    faces.extend([tuple(reversed(range(segments))), tuple(rings*segments+j for j in range(segments))])
    return Mesh(name, vertices, faces, material, bone)


def Plate(name, outline, depth, material, bone=None, bevel=.007):
    n = len(outline)
    vertices = [Vector(v) for v in outline]
    vertices += [Vector((v[0], v[1]+depth, v[2])) for v in outline]
    projected = [Vector((v[0], 0, v[2])) for v in outline]
    tris = tessellate_polygon([projected])
    faces = []
    for tri in tris:
        ids = tuple(p if isinstance(p, int) else
                    min(range(n), key=lambda i:(projected[i]-p).length_squared) for p in tri)
        faces.append(ids)
        faces.append(tuple(i+n for i in reversed(ids)))
    for i in range(n):
        j = (i+1)%n
        faces.append((i, j, j+n, i+n))
    return Mesh(name, vertices, faces, material, bone, bevel, smooth=False)


def Stroke(name, points, radius, material, bone=None):
    verts, faces = [], []
    for i, point in enumerate(points):
        tangent = Vector(points[min(i+1,len(points)-1)])-Vector(points[max(0,i-1)])
        d = tangent.normalized()
        u = d.cross(Vector((0, 1, 0))).normalized()
        if u.length < .1:
            u = Vector((1,0,0))
        v = d.cross(u).normalized()
        for j in range(6):
            angle = math.tau*j/6
            verts.append(Vector(point)+radius*(u*math.cos(angle)+v*math.sin(angle)))
    for i in range(len(points)-1):
        for j in range(6):
            faces.append((i*6+j, i*6+(j+1)%6, (i+1)*6+(j+1)%6, (i+1)*6+j))
    return Mesh(name, verts, faces, material, bone)


def Setup():
    global SCENE, MODEL, RIG
    SCENE = bpy.data.scenes['MiniBotC_Studio']
    bpy.context.window.scene = SCENE
    RIG = bpy.data.objects['MiniBotC_Humanoid']
    MODEL = bpy.data.collections.get('C_Model') or bpy.data.collections.new('C_Model')
    if MODEL.name not in SCENE.collection.children:
        SCENE.collection.children.link(MODEL)
    for name, color, metal, rough, glow in [
        ('Porcelain', (.79,.82,.79), .22,.29,0),
        ('Pearl', (.50,.57,.55), .65,.32,0),
        ('Graphite', (.032,.043,.044), .55,.36,0),
        ('Rubber', (.013,.019,.021), .12,.53,0),
        ('Titanium', (.22,.27,.28), .88,.27,0),
        ('Visor', (.008,.014,.018), .4,.14,0),
        ('Green', (.16,.85,.008), .24,.28,2.0),
        ('Eye', (.24,1.0,.012), .08,.22,3.5),
        ('DarkGreen', (.026,.12,.033), .65,.34,0),
        ('Steel', (.29,.36,.37), .94,.20,0),
        ('Brass', (.30,.23,.105), .85,.28,0)]:
        MAT[name] = Material(name,color,metal,rough,glow)


def Head():
    bone = 'Head'
    Loft('HelmetShell', [(1.81,.10,.12,0,.035), (1.84,.19,.205,0,.035),
         (1.92,.305,.272,0,.035), (2.065,.351,.292,0,.035),
         (2.19,.326,.277,0,.04), (2.285,.232,.214,0,.04),
         (2.327,.08,.082,0,.04)], 'Porcelain',bone,48)
    outline = [(-.298,2.145),(-.184,2.114),(0,2.057),(.184,2.114),(.298,2.145),
               (.286,2.009),(.18,1.901),(0,1.846),(-.18,1.901),(-.286,2.009)]
    Plate('VisorFrame', [(x,-.292+.7*x*x,z) for x,z in outline], .052,'Graphite',bone,.016)
    center = Vector((0,1.996))
    inner = []
    for x,z in outline:
        x*=.944
        z=1.996+(z-1.996)*.86
        inner.append((x,-.308+.74*x*x,z))
    Plate('VisorGlass',inner,.029,'Visor',bone,.012)
    for s in [-1,1]:
        side='L' if s==1 else 'R'
        eye=[(.058,2.006),(.101,2.032),(.251,2.105),(.239,2.040),(.178,1.981),(.115,1.973)]
        Plate('EyeSocket_'+side,[(s*x,-.315+.71*x*x,z) for x,z in eye],.008,'DarkGreen',bone,.008)
        ex=sum(x for x,z in eye)/len(eye)
        ez=sum(z for x,z in eye)/len(eye)
        Plate('EyeLens_'+side,[(s*(ex+(x-ex)*.88),-.322+.71*x*x,ez+(z-ez)*.78) for x,z in eye],
              .007,'Eye',bone,.009)
        cheek=[(.19,1.912),(.292,2.006),(.326,2.078),(.322,1.967),(.215,1.858),(.084,1.824)]
        Plate('CheekPlate_'+side,[(s*x,-.237+.43*x*x,z) for x,z in cheek],.07,'Porcelain',bone,.012)
        axis=(s,0,0)
        Cylinder('EarGasket_'+side,(s*.341,.037,2.086),axis,.137,.052,'Rubber',bone)
        Cylinder('EarHousing_'+side,(s*.372,.037,2.086),axis,.120,.045,'Pearl',bone)
        Cylinder('EarFace_'+side,(s*.397,.037,2.086),axis,.092,.009,'Graphite',bone)
        Cylinder('EarRing_'+side,(s*.404,.037,2.086),axis,.076,.005,'Green',bone)
        Cylinder('EarCore_'+side,(s*.409,.037,2.086),axis,.067,.008,'Titanium',bone)
        fin=[(.235,2.226),(.266,2.427),(.420,2.58),(.372,2.328),(.317,2.202)]
        Plate('SensorFin_'+side,[(s*x,-.009,z) for x,z in fin],.095,'Porcelain',bone,.009)
        inset=[(.273,2.293),(.296,2.421),(.397,2.531),(.351,2.349)]
        Plate('SensorInset_'+side,[(s*x,-.02,z) for x,z in inset],.012,'Graphite',bone,.004)
        green=[(.303,2.378),(.322,2.423),(.374,2.488),(.344,2.408)]
        Plate('SensorLight_'+side,[(s*x,-.026,z) for x,z in green],.006,'Green',bone,.002)
        Stroke('CrownSeam_'+side,[(s*.075,-.215,2.286),(s*.137,-.243,2.247),
                                (s*.19,-.262,2.193)],.0022,'Graphite',bone)
    Plate('ForeheadKeel',[(-.09,-.237,2.302),(0,-.243,2.327),(.09,-.237,2.302),
          (.125,-.288,2.163),(0,-.304,2.081),(-.125,-.288,2.163)],.025,'Porcelain',bone,.008)
    for z,r in [(1.71,.076),(1.75,.077),(1.785,.088)]:
        Cylinder('Neck_'+str(z),(0,0,z),(0,0,1),r,.035,'Graphite','Neck')


def Torso():
    Loft('ThoraxUnderframe',[(1.325,.145,.100,0,.015),(1.40,.21,.137,0,.02),
         (1.55,.257,.14,0,.025),(1.645,.19,.10,0,.02)],'Graphite','UpperChest',24,square=.8)
    Loft('PelvisChassis',[(1.015,.119,.104,0,.018),(1.09,.205,.14,0,.01),
         (1.16,.213,.139,0,.01),(1.195,.149,.105,0,0)],'Graphite','Hips',24,square=.72)
    Plate('PelvisApron',[(-.137,-.136,1.17),(0,-.171,1.118),(.137,-.136,1.17),
          (.104,-.176,1.025),(0,-.19,.971),(-.104,-.176,1.025)],.047,'Porcelain','Hips',.014)
    for i,(z,rx,ry) in enumerate([(1.18,.137,.105),(1.23,.142,.108),(1.28,.145,.11),(1.33,.15,.111)]):
        Loft('AbdomenSegment_'+str(i),[(z,rx*.96,ry*.96,0,.01),(z+.009,rx,ry,0,.01),
             (z+.041,rx*.96,ry*.96,0,.013)],'Graphite','Spine' if i<2 else 'Chest',24,square=.75)
        Stroke('AbdomenRim_'+str(i),[(-rx*.75,-ry*.80,z+.012),(0,-ry,z+.006),
               (rx*.75,-ry*.80,z+.012)],.003,'Titanium','Spine' if i<2 else 'Chest')
    for s in [-1,1]:
        side='L' if s==1 else 'R'
        shape=[(.029,1.58),(.12,1.625),(.234,1.60),(.269,1.49),(.173,1.387),(.029,1.358)]
        Plate('Pectoral_'+side,[(s*x,-.17-.17*(1-x/.28),z) for x,z in shape],
              .083,'Porcelain','UpperChest',.021)
        Stroke('ChestSeam_'+side,[(s*.062,-.307,1.492),(s*.155,-.267,1.522),
               (s*.237,-.21,1.553)],.0027,'Graphite','UpperChest')
        Plate('HipFlank_'+side,[(s*.157,-.10,1.18),(s*.234,-.07,1.13),
              (s*.246,-.06,1.025),(s*.17,-.126,1.06)],.15,'Porcelain','Hips',.012)
        Plate('BackPlate_'+side,[(s*.02,.178,1.59),(s*.18,.13,1.61),
              (s*.24,.105,1.48),(s*.12,.171,1.366),(s*.02,.189,1.40)],-.051,'Porcelain','UpperChest',.016)
    Plate('CoreSurround',[(-.076,-.33,1.565),(.076,-.33,1.565),(.058,-.347,1.437),
          (0,-.355,1.40),(-.058,-.347,1.437)],.054,'Titanium','UpperChest',.012)
    Plate('CoreRecess',[(-.050,-.349,1.54),(.050,-.349,1.54),(.034,-.363,1.45),
          (0,-.366,1.426),(-.034,-.363,1.45)],.022,'Graphite','UpperChest',.007)
    Plate('CoreLight',[(-.031,-.365,1.522),(.031,-.365,1.522),(.021,-.375,1.465),
          (0,-.377,1.45),(-.021,-.375,1.465)],.008,'Green','UpperChest',.005)
    Plate('BackServiceRecess',[(-.06,.202,1.57),(.06,.202,1.57),(.052,.213,1.425),
          (-.052,.213,1.425)],-.025,'Graphite','UpperChest',.009)
    Plate('BackStatus',[(-.012,.217,1.543),(.012,.217,1.543),(.012,.225,1.467),
          (-.012,.225,1.467)],-.008,'Green','UpperChest',.003)


def Limbs():
    for prefix,s in [('Left',1),('Right',-1)]:
        for stem,radius in [('Arm',.078),('ForeArm',.090),('UpLeg',.113),('Leg',.107)]:
            bone=prefix+stem
            b=RIG.data.bones[bone]
            a,t=b.head_local,b.tail_local
            Axial(stem+'Inner_'+prefix,a,t,[(.02,radius*.60,radius*.60),
                  (.98,radius*.58,radius*.58)],'Rubber',bone,16)
            profile=[(.13,radius*.70,radius*.64),(.22,radius*.97,radius*.87),
                     (.43,radius,radius*.90),(.71,radius*.76,radius*.74),(.84,radius*.59,radius*.6)]
            if stem=='Arm':
                profile=[(.26,radius*.76,radius*.76),(.34,radius,radius*.90),
                         (.58,radius*.91,radius*.80),(.73,radius*.69,radius*.67)]
            Axial(stem+'Armor_'+prefix,a,t,profile,'Porcelain',bone,20,.78,.005)
            for k,p in enumerate([.08,.90]):
                center=a.lerp(t,p)
                Cylinder(stem+'Collar_'+prefix+str(k),center,t-a,radius*.67,.025,'Titanium',bone,20)
        for stem,radius in [('Arm',.092),('ForeArm',.062),('Hand',.050),('UpLeg',.096),('Leg',.084),('Foot',.069)]:
            bone=prefix+stem
            center=RIG.data.bones[bone].head_local
            Ball(stem+'Joint_'+prefix,center,(radius,)*3,'Graphite',bone,20,10)
            if stem in ['ForeArm','Leg','Foot']:
                Cylinder(stem+'Hinge_'+prefix,center+Vector((s*radius*.85,0,0)),(s,0,0),
                         radius*.69,.017,'Titanium',bone,20)
                Cylinder(stem+'Hub_'+prefix,center+Vector((s*radius*1.0,0,0)),(s,0,0),
                         radius*.46,.010,'Graphite',bone,20)
                Cylinder(stem+'Status_'+prefix,center+Vector((s*radius*1.075,0,0)),(s,0,0),
                         radius*.19,.008,'Green',bone,16)
        a=RIG.data.bones[prefix+'Arm'].head_local
        b=RIG.data.bones[prefix+'Arm'].tail_local
        shoulder=a+Vector((s*.025,0,.02))
        Ball('ShoulderUnder_'+prefix,shoulder,(.142,.133,.125),'Titanium',prefix+'Arm',24,12)
        Axial('ShoulderArmor_'+prefix,a,b,[(-.10,.063,.075),(.0,.14,.133),(.16,.157,.144),
              (.30,.139,.129),(.38,.102,.097)],'Porcelain',prefix+'Arm',24,.91,.005)
        Cylinder('ShoulderOpticHousing_'+prefix,shoulder+Vector((0,-.132,.018)),(0,-1,0),
                 .055,.016,'Graphite',prefix+'Arm',24)
        Cylinder('ShoulderOptic_'+prefix,shoulder+Vector((0,-.142,.018)),(0,-1,0),
                 .035,.008,'Green',prefix+'Arm',24)
        Cylinder('ShoulderOpticCover_'+prefix,shoulder+Vector((0,-.15,.018)),(0,-1,0),
                 .026,.01,'Titanium',prefix+'Arm',24)
        knee=RIG.data.bones[prefix+'Leg'].head_local
        x,y,z=knee
        Plate('KneeShield_'+prefix,[(x-.067,y-.072,z+.055),(x+.067,y-.072,z+.055),
              (x+.076,y-.099,z-.037),(x,y-.126,z-.096),(x-.076,y-.099,z-.037)],
              .035,'Porcelain',prefix+'Leg',.011)
        calf=RIG.data.bones[prefix+'Leg'].head_local.lerp(RIG.data.bones[prefix+'Leg'].tail_local,.60)
        Stroke('ShinInset_'+prefix,[(calf.x-s*.025,calf.y-.086,calf.z+.094),
               (calf.x-s*.048,calf.y-.09,calf.z), (calf.x-s*.031,calf.y-.07,calf.z-.067)],
               .0031,'Titanium',prefix+'Leg')
        x=s*.23
        Loft('HeelSole_'+prefix,[(.018,.092,.13,x,.0),(.04,.105,.14,x,-.008),
             (.070,.104,.143,x,-.01)],'Rubber',prefix+'Foot',24,square=.55)
        Loft('Instep_'+prefix,[(.058,.099,.136,x,-.01),(.12,.096,.143,x,-.016),
             (.188,.073,.082,x,.010),(.204,.052,.052,x,.015)],'Porcelain',prefix+'Foot',24,square=.66)
        Loft('ToeSole_'+prefix,[(.018,.099,.107,x,-.225),(.038,.106,.115,x,-.229),
             (.065,.101,.110,x,-.231)],'Rubber',prefix+'ToeBase',24,square=.53)
        Loft('ToeArmor_'+prefix,[(.057,.100,.108,x,-.231),(.104,.096,.106,x,-.220),
             (.139,.07,.079,x,-.196)],'Porcelain',prefix+'ToeBase',24,square=.55)
        Stroke('ToeSplit_'+prefix,[(x,-.328,.063),(x,-.314,.103),(x,-.262,.131)],
               .0026,'Titanium',prefix+'ToeBase')
        hand=RIG.data.bones[prefix+'Hand']
        a,t=hand.head_local,hand.tail_local
        Axial('Palm_'+prefix,a,t,[(.1,.038,.03),(.32,.061,.034),(.9,.063,.031),
              (1.03,.045,.026)],'Graphite',prefix+'Hand',16,.5,.003)
        Axial('HandBackArmor_'+prefix,a+Vector((0,.02,0)),t+Vector((0,.02,0)),
              [(.22,.037,.023),(.38,.057,.028),(.78,.058,.026),(.96,.044,.02)],
              'Porcelain',prefix+'Hand',16,.55,.003)
        for finger in ['Thumb','Index','Middle','Ring','Pinky']:
            for j in range(1,4):
                bone=f'{prefix}Hand{finger}{j}'
                fb=RIG.data.bones[bone]
                radius=.018 if finger=='Thumb' else (.014 if finger=='Pinky' else .0165)
                Ball('Knuckle_'+bone,fb.head_local,(radius,)*3,'Titanium',bone,12,6)
                Axial('Phalange_'+bone,fb.head_local,fb.tail_local,
                      [(.12,radius*.85,radius*.80),(.28,radius,radius*.90),
                       (.81,radius*.90,radius*.84),(.96,radius*.73,radius*.7)],
                      'Graphite',bone,12,.75,.001)
                Axial('FingerPlate_'+bone,fb.head_local+Vector((0,.007,0)),
                      fb.tail_local+Vector((0,.007,0)),[(.23,radius*.78,radius*.53),
                      (.72,radius*.82,radius*.54)],'Porcelain',bone,8,.65,.001)


def BuildModel():
    Setup()
    Head()
    Torso()
    Limbs()
    bpy.context.view_layer.update()
    return PARTS


def Sword():
    collection=bpy.data.collections.new('C_Weapon')
    SCENE.collection.children.link(collection)
    root=bpy.data.objects.new('C_Sword',None)
    collection.objects.link(root)
    root.empty_display_size=.07
    root['blade_length_m']=1.10
    root['grip_length_m']=.25
    root['weapon_count']=1
    initial=set(PARTS)
    sections=[]
    for i in range(13):
        t=i/12
        width=.059*(1-.18*t)
        if t>.85:
            width*=max(.025,(1-t)/.15)
        sections.append((t, -.145*t*t, width))
    vertices=[]
    for t,c,w in sections:
        for x,y in [(c-w,-.008),(c+w,-.010),(c+w,.010),(c-w,.008)]:
            vertices.append((x,y,.03+1.10*t))
    faces=[]
    for i in range(12):
        for j in range(4):
            faces.append((4*i+j,4*i+(j+1)%4,4*(i+1)+(j+1)%4,4*(i+1)+j))
    faces.extend([(3,2,1,0),(48,49,50,51)])
    Mesh('BladeSteel',vertices,faces,'Steel',bevel=.0015)
    for sign in [-1,1]:
        verts=[]
        for t,c,w in sections:
            verts.extend([(c+w*.47,sign*.0125,.03+1.10*t),(c+w,sign*.0105,.03+1.10*t)])
        Mesh('BladeGreenEdge_'+str(sign),verts,[(2*i,2*i+1,2*i+3,2*i+2) for i in range(12)],'Green',smooth=False)
        verts=[]
        for t,c,w in sections[:-1]:
            verts.extend([(c-w*.4,sign*.013,.05+1.08*t),(c-w*.15,sign*.013,.05+1.08*t)])
        Mesh('BladeFuller_'+str(sign),verts,[(2*i,2*i+1,2*i+3,2*i+2) for i in range(11)],'Graphite',smooth=False)
    Cylinder('BladeCollar',(0,0,.028),(0,0,1),.045,.06,'Titanium',segments=8)
    Plate('SwordGuard',[(-.10,-.039,.017),(-.105,-.039,-.013),(-.032,-.042,-.035),
          (.076,-.042,-.024),(.10,-.039,.008),(.085,-.039,.028)],.078,'Brass',bevel=.008)
    Cylinder('SwordGrip',(0,0,-.132),(0,0,1),.025,.245,'Rubber',segments=16)
    for i in range(10):
        Cylinder('GripBand_'+str(i),(0,0,-.023-i*.022),(0,0,1),.027,.007,'Graphite',segments=16)
    Cylinder('SwordPommel',(0,0,-.266),(0,0,1),.034,.028,'Titanium',segments=12)
    Cylinder('PommelLight',(0,0,-.284),(0,0,1),.014,.005,'Green',segments=16)
    for obj in set(PARTS)-initial:
        for col in list(obj.users_collection):
            col.objects.unlink(obj)
        collection.objects.link(obj)
        obj.parent=root
    for name,z in [('C_GripRight',-.065),('C_GripLeft',-.165),('C_BladeTip',1.13)]:
        o=bpy.data.objects.new(name,None)
        collection.objects.link(o)
        o.parent=root
        o.location=(0,0,z)
        o.empty_display_size=.025
        o.hide_render=True
    direction=Vector((-.64,-.12,-.76)).normalized()
    orientation=Vector((0,0,1)).rotation_difference(direction).to_matrix().to_4x4()
    hand=RIG.data.bones['RightHand']
    grip=hand.head_local.lerp(hand.tail_local,.65)+Vector((0,-.025,0))
    orientation.translation=grip+direction*.065
    root.matrix_world=orientation
    bpy.context.view_layer.update()
    constraint=root.constraints.new('CHILD_OF')
    constraint.name='Right hand owns the sword'
    constraint.target=RIG
    constraint.subtarget='RightHand'
    constraint.inverse_matrix=(RIG.matrix_world@RIG.pose.bones['RightHand'].matrix).inverted()
    bpy.context.view_layer.update()
    return root


def Studio():
    studio=bpy.data.collections.new('C_Studio')
    SCENE.collection.children.link(studio)
    floor_material=Material('StudioFloor',(.13,.16,.17),.05,.6)
    MAT['StudioFloor']=floor_material
    Mesh('StudioFloor',[(-200,-200,-.003),(200,-200,-.003),(200,200,-.003),(-200,200,-.003)],
         [(0,1,2,3)],'StudioFloor',collection=studio,smooth=False)
    world=bpy.data.worlds.new('C_StudioWorld')
    world.use_nodes=True
    bg=next(n for n in world.node_tree.nodes if n.type=='BACKGROUND')
    bg.inputs['Color'].default_value=(.19,.225,.26,1)
    bg.inputs['Strength'].default_value=.45
    SCENE.world=world
    for name,loc,power,size,color in [
        ('Key',(-3.4,-4.2,5.3),850,4.0,(1.0,.94,.84)),
        ('Fill',(3.0,-2.4,3.1),650,3.0,(.80,.89,1.0)),
        ('Rim',(1.8,2.0,4.3),1050,2.6,(.77,1.0,.88)),
        ('Top',(-.4,.2,5.0),350,2.0,(1.0,1.0,1.0))]:
        data=bpy.data.lights.new('C_'+name,'AREA')
        obj=bpy.data.objects.new('C_'+name,data)
        studio.objects.link(obj)
        obj.location=loc
        obj.rotation_euler=(Vector((0,0,1.25))-obj.location).to_track_quat('-Z','Y').to_euler()
        data.energy=power
        data.shape='DISK'
        data.size=size
        data.color=color
    for name,loc,scale in [('Hero',(3.1,-6.2,2.8),3.22),('Front',(0,-8,1.3),3.12),
                           ('Right',(-8,0,1.3),3.12),('Back',(0,8,1.3),3.12)]:
        data=bpy.data.cameras.new('C_Camera_'+name)
        obj=bpy.data.objects.new('C_Camera_'+name,data)
        studio.objects.link(obj)
        obj.location=loc
        obj.rotation_euler=(Vector((-.06,0,1.28))-obj.location).to_track_quat('-Z','Y').to_euler()
        data.type='ORTHO'
        data.ortho_scale=scale
        data.lens=55
        if name=='Hero':
            SCENE.camera=obj
    SCENE.render.engine='CYCLES'
    SCENE.cycles.samples=32
    SCENE.cycles.use_denoising=True
    SCENE.render.resolution_x=1100
    SCENE.render.resolution_y=1100
    SCENE.render.resolution_percentage=100
    SCENE.render.image_settings.file_format='PNG'
    SCENE.view_settings.view_transform='AgX'
    SCENE.view_settings.look='AgX - Medium High Contrast'
    SCENE.render.film_transparent=False
    SCENE.render.use_file_extension=True
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type=='VIEW_3D':
                area.spaces.active.clip_end=200
                area.spaces.active.region_3d.view_distance=4.2
                area.spaces.active.region_3d.view_location=(0,0,1.25)
                area.spaces.active.region_3d.view_rotation=SCENE.camera.rotation_euler.to_quaternion()
                area.spaces.active.shading.type='MATERIAL'
                area.spaces.active.overlay.show_overlays=False
    bpy.context.view_layer.update()
