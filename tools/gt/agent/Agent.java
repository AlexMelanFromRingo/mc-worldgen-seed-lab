import java.lang.classfile.*;
import java.lang.constant.*;
import java.lang.instrument.*;
import java.security.ProtectionDomain;

/** Java-агент отладки (только для локальных экспериментов с НАСТОЯЩИМ сервером): перед PlaceOnGroundDecorator.attemptToPlaceBlockAbove
 *  вызывает Dbg.pog(контекст, позиция) — запись того, что видит игра. Код Mojang не изменяется на диске, преобразование — в памяти. */
public class Agent {
    public static void premain(String args, Instrumentation inst) {
        try {   // Dbg должен быть виден загрузчику классов игры (у бандлера свой URLClassLoader) — кладём агент в путь загрузчика начальной загрузки
            String path = new java.io.File(Agent.class.getProtectionDomain().getCodeSource().getLocation().toURI()).getParent() + "/dbgboot.jar";
            inst.appendToBootstrapClassLoaderSearch(new java.util.jar.JarFile(path));
        } catch (Exception e) { throw new RuntimeException(e); }
        inst.addTransformer(new ClassFileTransformer() {
            @Override public byte[] transform(Module m, ClassLoader l, String name, Class<?> c, ProtectionDomain pd, byte[] bytes) {
                final boolean wgr = "net/minecraft/server/level/WorldGenRegion".equals(name);
                final boolean msh = "net/minecraft/world/level/block/MushroomBlock".equals(name);
                final boolean chk = "net/minecraft/world/level/chunk/LevelChunk".equals(name) || ((System.getProperty("dbg.protoset") != null || System.getProperty("dbg.rm") != null) && "net/minecraft/world/level/chunk/ProtoChunk".equals(name));
                final boolean mrk = "net/minecraft/world/level/chunk/ProtoChunk".equals(name);
                if (!wgr && !msh && !chk && !mrk && !"net/minecraft/world/level/levelgen/feature/treedecorators/PlaceOnGroundDecorator".equals(name)) return null;
                try {
                    ClassFile cf = ClassFile.of(ClassFile.ClassHierarchyResolverOption.of(
                        ClassHierarchyResolver.ofResourceParsing(l).orElse(ClassHierarchyResolver.defaultResolver())));
                    ClassModel cm = cf.parse(bytes);
                    ClassTransform tChk = ClassTransform.transformingMethodBodies(
                        mm -> mm.methodName().equalsString("setBlockState") && mm.methodTypeSymbol().parameterCount() == 3,
                        new CodeTransform() {
                            boolean done = false;
                            @Override public void accept(CodeBuilder b, CodeElement e) {
                                if (!done) {
                                    done = true;
                                    b.aload(0).aload(1).aload(2).iload(3).invokestatic(ClassDesc.of("Dbg"), "chunkSet",
                                        MethodTypeDesc.of(ConstantDescs.CD_void, ConstantDescs.CD_Object, ConstantDescs.CD_Object, ConstantDescs.CD_Object, ConstantDescs.CD_int));
                                }
                                b.with(e);
                            }
                        });
                    ClassTransform tMrk = ClassTransform.transformingMethodBodies(
                        mm -> mm.methodName().equalsString("markPosForPostProcessing"),
                        new CodeTransform() {
                            boolean done = false;
                            @Override public void accept(CodeBuilder b, CodeElement e) {
                                if (!done) {
                                    done = true;
                                    b.aload(0).aload(1).invokestatic(ClassDesc.of("Dbg"), "mark",
                                        MethodTypeDesc.of(ConstantDescs.CD_void, ConstantDescs.CD_Object, ConstantDescs.CD_Object));
                                }
                                b.with(e);
                            }
                        });
                    if (chk || mrk) {
                        ClassTransform t = null;
                        if (chk) t = tChk;
                        if (name.endsWith("ProtoChunk") && System.getProperty("dbg.protoset") == null && System.getProperty("dbg.rm") == null) t = tMrk;
                        if (t == null) return null;
                        return cf.transformClass(cm, t);
                    }
                    if (msh) return cf.transformClass(cm, ClassTransform.transformingMethodBodies(
                        mm -> mm.methodName().equalsString("canSurvive"),
                        new CodeTransform() {
                            boolean done = false;
                            @Override public void accept(CodeBuilder b, CodeElement e) {
                                if (!done) {
                                    done = true;
                                    b.aload(2).aload(3).invokestatic(ClassDesc.of("Dbg"), "cs",
                                        MethodTypeDesc.of(ConstantDescs.CD_void, ConstantDescs.CD_Object, ConstantDescs.CD_Object));
                                }
                                b.with(e);
                            }
                        }));
                    if (wgr) return cf.transformClass(cm, ClassTransform.transformingMethodBodies(
                        mm -> mm.methodName().equalsString("setBlock") && mm.methodTypeSymbol().parameterCount() == 4,
                        new CodeTransform() {
                            boolean done = false;
                            @Override public void accept(CodeBuilder b, CodeElement e) {
                                if (!done) {
                                    done = true;
                                    b.aload(0).aload(1).aload(2).iload(3).invokestatic(ClassDesc.of("Dbg"), "set",
                                        MethodTypeDesc.of(ConstantDescs.CD_void, ConstantDescs.CD_Object, ConstantDescs.CD_Object, ConstantDescs.CD_Object, ConstantDescs.CD_int));
                                }
                                b.with(e);
                            }
                        }));
                    return cf.transformClass(cm, ClassTransform.transformingMethodBodies(
                        mm -> mm.methodName().equalsString("attemptToPlaceBlockAbove"),
                        new CodeTransform() {
                            boolean done = false;
                            @Override public void accept(CodeBuilder b, CodeElement e) {
                                if (!done) {
                                    done = true;
                                    b.aload(1).aload(2).invokestatic(ClassDesc.of("Dbg"), "pog",
                                        MethodTypeDesc.of(ConstantDescs.CD_void, ConstantDescs.CD_Object, ConstantDescs.CD_Object));
                                }
                                b.with(e);
                            }
                        }));
                } catch (Throwable t) { t.printStackTrace(); return null; }
            }
        });
    }
}
