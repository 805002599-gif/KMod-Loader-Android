import com.android.tools.smali.dexlib2.*;
import com.android.tools.smali.dexlib2.iface.*;
import com.android.tools.smali.dexlib2.iface.instruction.*;
import com.android.tools.smali.dexlib2.iface.reference.*;
import com.android.tools.smali.dexlib2.immutable.*;
import com.android.tools.smali.dexlib2.immutable.instruction.*;
import com.android.tools.smali.dexlib2.immutable.reference.*;
import com.android.tools.smali.dexlib2.writer.pool.DexPool;
import java.io.File;
import java.util.*;

// usage: inspect <dex> <classDesc>   |   patch <in.dex> <out.dex> <classDesc> <libname>
public class DexTool {
    public static void main(String[] a) throws Exception {
        Opcodes ops = Opcodes.forApi(35);
        DexFile dex = DexFileFactory.loadDexFile(new File(a[1]), ops);
        if (a[0].equals("inspect")) {
            for (ClassDef c : dex.getClasses()) {
                if (!c.getType().equals(a[2])) continue;
                System.out.println("class " + c.getType() + " super " + c.getSuperclass() + " flags " + c.getAccessFlags());
                for (Method m : c.getMethods()) {
                    MethodImplementation impl = m.getImplementation();
                    System.out.println("  " + m.getName() + m.getParameterTypes() + m.getReturnType() + " regs=" + (impl == null ? -1 : impl.getRegisterCount()));
                    if (m.getName().equals("<clinit>") && impl != null)
                        for (Instruction i : impl.getInstructions()) System.out.println("     " + i.getOpcode());
                }
            }
            return;
        }
        // patch
        String target = a[3], lib = a[4];
        DexPool pool = new DexPool(Opcodes.forApi(28));
        boolean done = false;
        for (ClassDef c : dex.getClasses()) {
            if (!c.getType().equals(target)) { pool.internClass(c); continue; }
            List<Method> ms = new ArrayList<>();
            boolean hadClinit = false;
            for (Method m : c.getMethods()) {
                if (m.getName().equals("<clinit>")) {
                    hadClinit = true;
                    MethodImplementation impl = m.getImplementation();
                    int regs = Math.max(impl.getRegisterCount(), 1);
                    List<Instruction> ins = new ArrayList<>();
                    ins.add(new ImmutableInstruction21c(Opcode.CONST_STRING, 0, new ImmutableStringReference(lib)));
                    ins.add(new ImmutableInstruction35c(Opcode.INVOKE_STATIC, 1, 0, 0, 0, 0, 0,
                        new ImmutableMethodReference("Ljava/lang/System;", "loadLibrary", Arrays.asList("Ljava/lang/String;"), "V")));
                    // original instructions follow; their registers are unchanged (v0 clobbered only before they run)
                    for (Instruction i : impl.getInstructions()) ins.add(ImmutableInstruction.of(i));
                    // try blocks / debug items use code-unit offsets -> shift; avoid by refusing if present
                    if (!impl.getTryBlocks().isEmpty()) throw new RuntimeException("clinit has try blocks; handle manually");
                    ImmutableMethodImplementation ni = new ImmutableMethodImplementation(regs, ins, null, null);
                    ms.add(new ImmutableMethod(m.getDefiningClass(), m.getName(), m.getParameters(), m.getReturnType(), m.getAccessFlags(), m.getAnnotations(), m.getHiddenApiRestrictions(), ni));
                } else ms.add(m);
            }
            if (!hadClinit) {
                List<Instruction> ins = new ArrayList<>();
                ins.add(new ImmutableInstruction21c(Opcode.CONST_STRING, 0, new ImmutableStringReference(lib)));
                ins.add(new ImmutableInstruction35c(Opcode.INVOKE_STATIC, 1, 0, 0, 0, 0, 0,
                    new ImmutableMethodReference("Ljava/lang/System;", "loadLibrary", Arrays.asList("Ljava/lang/String;"), "V")));
                ins.add(new ImmutableInstruction10x(Opcode.RETURN_VOID));
                ImmutableMethodImplementation ni = new ImmutableMethodImplementation(1, ins, null, null);
                ms.add(new ImmutableMethod(c.getType(), "<clinit>", Collections.<MethodParameter>emptyList(), "V",
                    AccessFlags.STATIC.getValue() | AccessFlags.CONSTRUCTOR.getValue(), null, null, ni));
            }
            pool.internClass(new ImmutableClassDef(c.getType(), c.getAccessFlags(), c.getSuperclass(), c.getInterfaces(),
                c.getSourceFile(), c.getAnnotations(), c.getFields(), ms));
            done = true;
            System.out.println("patched " + c.getType() + " (had clinit: " + hadClinit + ")");
        }
        if (!done) throw new RuntimeException("class not found");
        pool.writeTo(new com.android.tools.smali.dexlib2.writer.io.FileDataStore(new File(a[2])));
    }
}
