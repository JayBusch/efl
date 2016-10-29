
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
public struct ClassDescription
{
    public uint version;
    [MarshalAs(UnmanagedType.LPStr)] public String name;
    public int class_type;
    public UIntPtr data_size;
    public IntPtr class_initializer;
    public IntPtr class_constructor;
    public IntPtr class_destructor;
}

public struct Evas_Object_Box_Layout {};
public struct Eina_Free_Cb {};
public struct Evas_Object_Box_Option {};

namespace eina {
    
public struct Rw_Slice {}
public struct Slice {}

}

namespace efl { namespace kw_event {

public struct Description {};
        
}

public struct Callback_Priority {};
public struct Event_Cb {};
public struct Callback_Array_Item {};
public struct Dbg_Info {};

}

namespace efl { namespace gfx {

public interface Buffer {}

namespace buffer {
public struct Access_Mode {}
}
        
} }

namespace evas { namespace font {

public struct Hinting_Flags {}
        
}

public struct Modifier_Mask {}

public struct Coord {}

public struct Touch_Point_State {}

}

