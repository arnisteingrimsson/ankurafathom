"""Explicit local project adapters; no executable commands accepted from clients."""
PROJECTS = {
    'equinix-ai': dict(
        name='AI Opportunity Lab', client='Equinix discussion', domain='AI infrastructure',
        description='Explore how demand, solution design and site readiness turn opportunities into active customers.',
        folder='projects/equinix-ai', spec='projects/equinix-ai/spec.py', source='projects/equinix-ai/native/model.cpp',
        runner='projects/equinix-ai/native/build/equinix-model',
        build='projects/equinix-ai/build.py', server='projects/equinix-ai/server.py',
        port=8088, health='/api/catalog', version='equinix-ai-1', time_unit='day',
        state='artifacts/equinix-ai-state',
        run_roots={'customer': 'artifacts/equinix-ai-state', 'acceptance': 'artifacts/equinix-ai-acceptance/runs'},
        verification='artifacts/equinix-ai-acceptance/verification.json', prerequisites=[],
        inputs=[
            dict(name='Sites and partner profiles', status='Assumed', description='Three fictional sites; identical initial provider profiles. No hardware benchmarks.'),
            dict(name='Customer demand and delivery', status='Assumed', description='Arrival, design, procurement, commissioning and patience distributions.'),
            dict(name='Operational validation', status='Not calibrated', description='Future CRM, deployment, capacity, revenue and energy observations.')]),
    'tr-operating': dict(
        name='T&R Decision Lab', client='Ankura practice growth', domain='Professional services',
        description='Explore people, AI adoption, project delivery and business development in a 150-person practice.',
        folder='projects/tr-ui', spec='projects/tr-ui/workshop/spec.py', source='projects/tr-ui/native/workshop_runner.cpp',
        runner='projects/tr-ui/native/build/tr-workshop',
        build='projects/tr-ui/workshop/build.py', server='projects/tr-ui/server.py',
        port=8087, health='/workshop/catalog', version='tr-operating-1', time_unit='hour',
        state='artifacts/tr-ui-state',
        run_roots={'customer': 'artifacts/tr-ui-state/operating-sessions', 'acceptance': 'artifacts/tr-operating-acceptance/sessions'},
        verification='artifacts/tr-operating-acceptance/verification.json',
        prerequisites=['build-arrow/fathom', 'projects/tr-ui/native/build/tr-live', 'artifacts/tr-pilot-150-validated-20260930/batch-00/inputs/model.json'],
        inputs=[
            dict(name='Workforce and skills', status='Assumed', description='150 illustrative employees across eight levels; skill and proficiency distributions.'),
            dict(name='Work and AI behavior', status='Assumed', description='Three delivery stages, hourly/fixed fees, learning and delayed BD feedback.'),
            dict(name='Operational validation', status='Not calibrated', description='Future HR, utilization, project and AI-pilot observations.')]),
}
