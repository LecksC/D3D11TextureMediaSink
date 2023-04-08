using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace D3D11TextureMediaSinkDemoCSharp
{
    static class Program
    {
        /// <summary>
        /// This is the main entry point of the application.
        /// </summary>
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault( false );
            //Application.Run( new Form1() );
            using( var app = new Form1() )
                app.Run();
        }
    }
}
